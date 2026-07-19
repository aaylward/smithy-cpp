// Phase 8 slice 3 e2e, in-memory half (ADR-0016): the generated ChatClient
// drives the generated ChatServer's StreamRouter through an injected
// InMemoryWebSocketPair dialer — every streaming flow (bidi round trips,
// server push, clean closes both directions, the typed mid-stream error)
// plus the unary neighbor, with no wire underneath. The dialer hands the
// client one end of the pair and serves the other end by invoking the
// router's Serve() callback directly with the synthesized upgrade request,
// exactly how the serve callback runs behind a real transport.
//
// chat_e2e_beast_test.cc is the real-WebSocket half (the PLAN §Phase 8 exit
// criterion). Note the link reality: a generated streaming client carries
// the default Beast dialer's definition (//runtime:http_beast), so even this
// wire-free binary joins the Beast targets behind the documented local
// exclusion list (docs/development.md); CI runs it everywhere.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "smithy/client/config.h"
#include "smithy/http/loopback.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace example::chat {
namespace {

constexpr char kModerator[] = "moderator";

// Reference handler: greets each joiner, echoes messages back to the room,
// kicks anyone who asks, and announces leavers before closing its side.
class RoomHandler final : public ChatHandler {
 public:
  smithy::Outcome<ListRoomsOutput> ListRooms(const ListRoomsInput&,
                                             const smithy::server::RequestContext&) override {
    ListRoomsOutput output;
    output.rooms.push_back(RoomSummary{.name = "lobby", .members = 2});
    return output;
  }

  smithy::Outcome<smithy::Unit> Converse(
      const ConverseInput& input, smithy::eventstream::EventStream<RoomEvents, ChatEvents>& stream,
      const smithy::server::RequestContext&) override {
    // The nickname rode the upgrade request's headers; announce the joiner.
    MemberJoined joined;
    joined.member = input.nickname.value_or("anonymous");
    if (!stream.Send(RoomEvents::FromJoined(joined)).ok()) return smithy::Unit{};
    while (true) {
      auto event = stream.Receive();
      if (!event.ok()) return smithy::Unit{};          // wire failed: nothing to add
      if (!event->has_value()) return smithy::Unit{};  // client closed cleanly
      const ChatEvents& received = **event;
      if (received.is_message()) {
        const ChatMessage& message = received.as_message();
        if (message.text == "kick me") {
          // The modeled mid-stream error: one exception message, then close.
          smithy::Error kicked = smithy::Error::Modeled("Kicked", "kicked from " + input.room);
          kicked.set_detail(Kicked{.message = "kicked from " + input.room, .by = kModerator});
          return kicked;
        }
        ChatMessage broadcast;
        broadcast.text = message.text;
        broadcast.sender = message.sender.value_or(joined.member);
        if (!stream.Send(RoomEvents::FromMessage(broadcast)).ok()) return smithy::Unit{};
      } else if (received.is_leave()) {
        MemberLeft left;
        left.member = joined.member;
        (void)stream.Send(RoomEvents::FromLeft(left));
        return smithy::Unit{};  // the server's side of a clean close
      }
    }
  }

  smithy::Outcome<smithy::Unit> Watch(
      const WatchInput& input,
      smithy::eventstream::EventStream<RoomEvents, smithy::eventstream::NoEvents>& stream,
      const smithy::server::RequestContext&) override {
    for (int i = 0; i < 3; ++i) {
      ChatMessage message;
      message.text = input.room + "-update-" + std::to_string(i);
      message.sender = "room";
      if (!stream.Send(RoomEvents::FromMessage(message)).ok()) return smithy::Unit{};
    }
    return smithy::Unit{};  // push, then close
  }
};

class ChatEndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<ChatServer>(std::make_shared<RoomHandler>());
    auto loopback = std::make_shared<smithy::http::Loopback>();
    ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = loopback;  // the unary neighbor's transport
    // The streaming seam (ADR-0016): hand the client one end of an in-memory
    // pair and serve the other end through the generated StreamRouter,
    // synthesizing the upgrade request a real transport would deliver.
    config.websocket_dialer = [this](const smithy::http::WebSocketDialRequest& request)
        -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
      last_dialed_target_ = request.target;
      auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
      smithy::http::HttpRequest upgrade;
      upgrade.method = "GET";
      upgrade.target = request.target;
      upgrade.headers = request.headers;
      server_sessions_.push_back(far);
      serve_threads_.emplace_back([serve = server_->StreamRouter()->Serve(), upgrade,
                                   session = far] { serve(upgrade, *session); });
      return near;
    };
    auto client = ChatClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<ChatClient>(std::move(*client));
  }

  void TearDown() override {
    // Close is idempotent; this unblocks any serve loop a failed test body
    // left mid-Receive, so the joins below cannot hang.
    for (auto& session : server_sessions_) session->Close();
    for (std::thread& thread : serve_threads_) thread.join();
  }

  std::unique_ptr<ChatServer> server_;
  std::unique_ptr<ChatClient> client_;
  std::vector<std::shared_ptr<smithy::http::WebSocket>> server_sessions_;
  std::vector<std::thread> serve_threads_;
  std::string last_dialed_target_;
};

TEST_F(ChatEndToEndTest, BidiEventsRoundTripThroughTheGeneratedPair) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  // The upgrade target resolved the @http bindings exactly like a unary call.
  EXPECT_EQ(last_dialed_target_, "/rooms/lobby/converse");

  auto joined = stream->Receive();
  ASSERT_TRUE(joined.ok() && joined->has_value());
  ASSERT_TRUE((**joined).is_joined());
  EXPECT_EQ((**joined).as_joined().member, "ada");

  for (int i = 0; i < 5; ++i) {
    ChatMessage message;
    message.text = "hello-" + std::to_string(i);
    message.sender = "ada";
    ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
    auto echo = stream->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_TRUE((**echo).is_message());
    EXPECT_EQ((**echo).as_message().text, "hello-" + std::to_string(i));
    EXPECT_EQ((**echo).as_message().sender, "ada");
  }

  // Client-initiated close: the handler sees the clean end and returns; the
  // acknowledging close surfaces here as Receive's nullopt.
  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, ServerEndsTheStreamAfterALeaveEvent) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "grace";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  LeaveNotice leave;
  leave.reason = "signing off";
  ASSERT_TRUE(stream->Send(ChatEvents::FromLeave(leave)).ok());
  auto left = stream->Receive();
  ASSERT_TRUE(left.ok() && left->has_value());
  ASSERT_TRUE((**left).is_left());
  EXPECT_EQ((**left).as_left().member, "grace");

  // Server-initiated close: the handler returned, so the next Receive is the
  // stream's natural end.
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, ServerPushStreamsWithoutAClientTransmitDirection) {
  WatchInput input;
  input.room = "lobby";
  auto stream = client_->Watch(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(last_dialed_target_, "/rooms/lobby/watch");

  for (int i = 0; i < 3; ++i) {
    auto update = stream->Receive();
    ASSERT_TRUE(update.ok() && update->has_value());
    ASSERT_TRUE((**update).is_message());
    EXPECT_EQ((**update).as_message().text, "lobby-update-" + std::to_string(i));
  }
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatEndToEndTest, UnaryOperationSharesTheService) {
  const auto rooms = client_->ListRooms();
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();
  ASSERT_EQ(rooms->rooms.size(), 1U);
  EXPECT_EQ(rooms->rooms[0].name, "lobby");
  EXPECT_EQ(rooms->rooms[0].members, 2);
}

TEST_F(ChatEndToEndTest, ModeledMidStreamErrorSurfacesTypedOnTheClient) {
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "mallory";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "kick me";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());

  // The exception message is terminal and unary-shaped: kind, code, message,
  // and the typed detail all match what a failed unary call would carry.
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Kicked");
  EXPECT_EQ(outcome.error().message(), "kicked from lobby");
  const Kicked* detail = outcome.error().detail<Kicked>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->by, kModerator);

  // The generated error matcher dispatches it like any modeled error.
  const ConverseErrors errors = ConverseErrors::FromError(outcome.error());
  ASSERT_TRUE(errors.is_kicked());
  EXPECT_EQ(errors.as_kicked().by, kModerator);
}

TEST_F(ChatEndToEndTest, GateRefusesAnUnknownStreamPath) {
  const auto gate = server_->StreamRouter()->Gate();
  smithy::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/nope";
  const auto refusal = gate(upgrade);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 404);

  // A routed upgrade is admitted (nullopt lets the transport upgrade).
  upgrade.target = "/rooms/lobby/converse";
  EXPECT_FALSE(gate(upgrade).has_value());
}

}  // namespace
}  // namespace example::chat
