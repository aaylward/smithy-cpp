// The PLAN §Phase 8 exit criterion: the generated chat client and the
// generated chat server complete full-duplex event streams over REAL
// WebSockets — BeastServerTransport upgrades on the StreamRouter's gate and
// serve callbacks, the generated client dials real loopback WebSockets
// through the default Beast dialer (no injection anywhere), and the same
// flows the in-memory chat_e2e_test pins run on the wire, TLS included.
//
// CI-only like weather_e2e_beast_test and streaming_compile_test: Boost
// doesn't fetch behind the documented download-blocking proxy, so the local
// sweep excludes this target (docs/development.md, "Machine-specific Bazel
// flags"); the same file compiles and runs against distro Boost + gtest with
// the g++ recipe documented there.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/chat/client.h"
#include "example/chat/server.h"
#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/tls_test_identity.h"

namespace example::chat {
namespace {

constexpr char kModerator[] = "moderator";

// The same reference handler chat_e2e_test drives through the in-memory
// pair: greets joiners, echoes messages, kicks on request, announces leavers.
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
    MemberJoined joined;
    joined.member = input.nickname.value_or("anonymous");
    if (!stream.Send(RoomEvents::FromJoined(joined)).ok()) return smithy::Unit{};
    while (true) {
      auto event = stream.Receive();
      if (!event.ok()) return smithy::Unit{};
      if (!event->has_value()) return smithy::Unit{};
      const ChatEvents& received = **event;
      if (received.is_message()) {
        const ChatMessage& message = received.as_message();
        if (message.text == "kick me") {
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
        return smithy::Unit{};
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
    return smithy::Unit{};
  }
};

class ChatBeastEndToEndTest : public testing::Test {
 protected:
  // The production wiring (ADR-0016): mount the generated StreamRouter on
  // the transport in two lines, unary dispatch untouched beside it.
  void Start(bool tls) {
    server_ = std::make_unique<ChatServer>(std::make_shared<RoomHandler>());
    smithy::http::BeastServerTransport::Options options;
    options.websocket_gate = server_->StreamRouter()->Gate();
    options.on_websocket = server_->StreamRouter()->Serve();
    if (tls) {
      options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
      options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
    }
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(options);
    ASSERT_TRUE(transport_->Start(server_->Handler()).ok());

    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    if (tls) {
      // One endpoint configures both directions: the unary transport and the
      // streaming dial derive wss/TLS from it (nothing configured twice).
      config.endpoint = "https://127.0.0.1:" + std::to_string(transport_->port());
      config.tls.ca_pem = smithy::testing::kTestCertificatePem;
      auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
      ASSERT_TRUE(http_client.ok()) << http_client.error().message();
      config.http_client = *http_client;
    } else {
      config.endpoint = "http://127.0.0.1:" + std::to_string(transport_->port());
    }
    auto client = ChatClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<ChatClient>(std::move(*client));
  }

  void TearDown() override {
    if (transport_ != nullptr) transport_->Stop();
  }

  std::unique_ptr<ChatServer> server_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
  std::unique_ptr<ChatClient> client_;
};

TEST_F(ChatBeastEndToEndTest, BidiEventsRoundTripOverRealWebSockets) {
  Start(/*tls=*/false);
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

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

  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatBeastEndToEndTest, ServerEndsTheStreamAfterALeaveEvent) {
  Start(/*tls=*/false);
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

  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(ChatBeastEndToEndTest, ServerPushStreamsOverTheUpgrade) {
  Start(/*tls=*/false);
  WatchInput input;
  input.room = "lobby";
  auto stream = client_->Watch(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

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

TEST_F(ChatBeastEndToEndTest, UnaryAndStreamingShareOnePort) {
  Start(/*tls=*/false);
  const auto rooms = client_->ListRooms();
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();
  ASSERT_EQ(rooms->rooms.size(), 1U);
  EXPECT_EQ(rooms->rooms[0].name, "lobby");

  auto stream = client_->Watch([] {
    WatchInput input;
    input.room = "lobby";
    return input;
  }());
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  auto update = stream->Receive();
  ASSERT_TRUE(update.ok() && update->has_value());
  stream->Close();
}

TEST_F(ChatBeastEndToEndTest, ModeledMidStreamErrorSurfacesTypedOnTheClient) {
  Start(/*tls=*/false);
  ConverseInput input;
  input.room = "lobby";
  input.nickname = "mallory";
  auto stream = client_->Converse(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "kick me";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());

  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Kicked");
  EXPECT_EQ(outcome.error().message(), "kicked from lobby");
  const Kicked* detail = outcome.error().detail<Kicked>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->by, kModerator);
  ASSERT_TRUE(ConverseErrors::FromError(outcome.error()).is_kicked());
}

TEST_F(ChatBeastEndToEndTest, GateRefusesAnUnknownStreamPathOnTheWire) {
  Start(/*tls=*/false);
  // No route matches /nope, so the gate answers 404 before any upgrade
  // exists and the dial fails.
  auto refused = smithy::http::BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = transport_->port(), .target = "/nope"});
  EXPECT_FALSE(refused.ok());
}

TEST_F(ChatBeastEndToEndTest, TlsStreamsCarryChatEndToEnd) {
  Start(/*tls=*/true);
  const auto rooms = client_->ListRooms();  // unary over https
  ASSERT_TRUE(rooms.ok()) << rooms.error().message();

  ConverseInput input;
  input.room = "lobby";
  input.nickname = "ada";
  auto stream = client_->Converse(input);  // streaming over wss
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(stream->Receive().ok());  // drain the joined greeting

  ChatMessage message;
  message.text = "over wss";
  ASSERT_TRUE(stream->Send(ChatEvents::FromMessage(message)).ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "over wss");
  stream->Close();
}

}  // namespace
}  // namespace example::chat
