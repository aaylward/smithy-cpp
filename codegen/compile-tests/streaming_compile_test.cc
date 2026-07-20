// The streaming halves of the compile harness, behaviorally (ADR-0016). A
// separate target from gauntlet_compile_test because generated streaming
// clients fall back to smithy::http::BeastWebSocketClient::Dialer(), so
// linking them pulls in Boost — this test joins the Beast targets behind the
// documented local exclusion list (docs/development.md); CI runs it. The
// injected in-memory dialer below is the ClientConfig::websocket_dialer seam
// working as designed: streams without a wire, exactly how consumer tests
// run them.
//
// Beyond compile coverage this drives the generated cbor streaming SERVER
// (never constructed anywhere else), the REST streaming route's
// validation-refusal arm (no other fixture has a constrained initial
// member), and the auth traits' upgrade-dial wiring. Link note: the client
// and server libraries of one service each carry an identical serde.cc; as
// static archives the linker resolves every serde symbol from the first and
// never pulls the second (the gauntlet_compile_test precedent) — compiling
// both serde TUs into one binary by hand would double-define them.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compile/streaming/cbor/client.h"
#include "compile/streaming/cbor/server.h"
#include "compile/streaming/rest/client.h"
#include "compile/streaming/rest/server.h"
#include "smithy/cbor/cbor.h"
#include "smithy/client/config.h"
#include "smithy/core/document.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/http/loopback.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace {

namespace relay = compile::streaming::rest;
namespace pipe = compile::streaming::cbor;

// A dialer handing out one end of an in-memory pair, keeping the other for
// the test to play the server and the full dial request for assertions.
smithy::ClientConfig InMemoryConfig(std::shared_ptr<smithy::http::WebSocket>* server_end,
                                    smithy::http::WebSocketDialRequest* dialed) {
  smithy::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [server_end, dialed](const smithy::http::WebSocketDialRequest& request)
      -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
    *dialed = request;
    auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
    *server_end = std::move(far);
    return near;
  };
  return config;
}

TEST(StreamingCompileTest, RestClientOpensAServerPushStream) {
  std::shared_ptr<smithy::http::WebSocket> server_end;
  smithy::http::WebSocketDialRequest dialed;
  auto client = relay::RelayClient::Create(InMemoryConfig(&server_end, &dialed));
  ASSERT_TRUE(client.ok());

  auto stream = client->Watch();
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/watch");

  server_end->Close();
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close
}

TEST(StreamingCompileTest, TheUpgradeDialCarriesAuthQueryAndHeaderBindings) {
  // The service's auth traits ride the upgrade dial exactly like a unary
  // request: @httpBearerAuth attaches the configured token, and the modeled
  // query/header initial members resolve onto the dial request.
  std::shared_ptr<smithy::http::WebSocket> server_end;
  smithy::http::WebSocketDialRequest dialed;
  smithy::ClientConfig config = InMemoryConfig(&server_end, &dialed);
  config.bearer_token = [] { return std::string("sesame"); };
  auto client = relay::RelayClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  relay::ConverseInput input;
  input.room = "lobby";
  input.since = 5;
  input.client = "probe";
  auto stream = client->Converse(input);
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/rooms/lobby/converse?since=5");
  EXPECT_EQ(dialed.headers.Get("authorization").value_or(""), "Bearer sesame");
  EXPECT_EQ(dialed.headers.Get("x-relay-client").value_or(""), "probe");
  stream->Close();
}

TEST(StreamingCompileTest, CborClientExchangesOnTheFixedUpgradeUri) {
  std::shared_ptr<smithy::http::WebSocket> server_end;
  smithy::http::WebSocketDialRequest dialed;
  smithy::ClientConfig config = InMemoryConfig(&server_end, &dialed);
  // @httpApiKeyAuth(in: "header"): the key rides the upgrade request.
  config.api_key = [] { return std::string("key-123"); };
  auto client = pipe::PipeClient::Create(std::move(config));
  ASSERT_TRUE(client.ok());

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed.target, "/service/Pipe/operation/Exchange");
  EXPECT_EQ(dialed.headers.Get("x-api-key").value_or(""), "key-123");

  pipe::ChatMessage message;
  message.text = "hello";
  message.sender = "ada";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());

  // The frame is the real envelope convention (ADR-0016): :event-type names
  // the engaged member, and the payload is the protocol's CBOR.
  auto frame = server_end->Receive();
  ASSERT_TRUE(frame.ok());
  ASSERT_TRUE(frame->has_value());
  auto envelope = smithy::eventstream::ParseEnvelope(**frame);
  ASSERT_TRUE(envelope.ok());
  EXPECT_EQ(envelope->kind, smithy::eventstream::EventEnvelope::Kind::kEvent);
  EXPECT_EQ(envelope->type, "message");
  EXPECT_EQ(envelope->content_type, "application/cbor");
  auto payload = smithy::cbor::Decode(envelope->payload);
  ASSERT_TRUE(payload.ok());
  ASSERT_TRUE(payload->is_map());
  const smithy::Document* text = payload->Find("text");
  ASSERT_NE(text, nullptr);
  ASSERT_TRUE(text->is_string());
  EXPECT_EQ(text->as_string(), "hello");
  const smithy::Document* sender = payload->Find("sender");
  ASSERT_NE(sender, nullptr);
  ASSERT_TRUE(sender->is_string());
  EXPECT_EQ(sender->as_string(), "ada");

  // The other direction: the test plays the server, minting the same
  // envelope shape; the generated decoder yields the typed event.
  smithy::DocumentMap joined;
  joined.emplace("member", smithy::Document("ada"));
  ASSERT_TRUE(server_end
                  ->Send(smithy::eventstream::MakeEventMessage(
                      "joined", "application/cbor",
                      smithy::cbor::Encode(smithy::Document(std::move(joined)))))
                  .ok());
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  ASSERT_TRUE(received->has_value());
  ASSERT_TRUE((**received).is_joined());
  EXPECT_EQ((**received).as_joined().member, "ada");

  stream->Close();
}

// ---------------------------------------------------------------------------
// The streaming route's validation-refusal arm: no other fixture has a
// constrained initial member (streaming.smithy's @length on the room label
// exists for exactly this). Serves an over-long label through the generated
// StreamRouter and expects one SerializationException exception message,
// then the close — and a handler that never ran.
// ---------------------------------------------------------------------------

class NeverCalledHandler final : public relay::RelayHandler {
 public:
  smithy::Outcome<smithy::Unit> Converse(const relay::ConverseInput&, relay::ConverseServerStream&,
                                         const smithy::server::RequestContext&) override {
    ADD_FAILURE() << "handler ran despite a validation failure";
    return smithy::Unit{};
  }
  smithy::Outcome<smithy::Unit> Watch(const relay::WatchInput&, relay::WatchServerStream& stream,
                                      const smithy::server::RequestContext&) override {
    stream.Close();
    return smithy::Unit{};
  }
};

TEST(StreamValidationTest, AnInvalidLabelRefusesWithOneExceptionThenTheClose) {
  relay::RelayServer server(std::make_shared<NeverCalledHandler>());
  auto [client_end, server_end] = smithy::http::InMemoryWebSocketPair::Create();
  smithy::http::HttpRequest upgrade;
  upgrade.method = "GET";
  upgrade.target = "/rooms/waytoolongroom/converse";  // @length(max: 8) violated
  std::thread serve_thread([serve = server.StreamRouter()->Serve(), upgrade, session = server_end] {
    serve(upgrade, *session);
  });

  auto refusal = client_end->Receive();
  ASSERT_TRUE(refusal.ok() && refusal->has_value());
  auto envelope = smithy::eventstream::ParseEnvelope(**refusal);
  ASSERT_TRUE(envelope.ok()) << envelope.error().message();
  EXPECT_EQ(envelope->kind, smithy::eventstream::EventEnvelope::Kind::kException);
  EXPECT_EQ(envelope->type, "SerializationException");

  auto closed = client_end->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
  serve_thread.join();
}

// ---------------------------------------------------------------------------
// The generated rpcv2Cbor streaming SERVER, behaviorally: nothing else ever
// constructs a PipeServer or serves its StreamRouter (gauntlet_compile_test
// only compiles it). The generated cbor client drives it through the
// in-memory pair: the fixed upgrade URI route, both codec directions over
// CBOR payloads, and the mid-stream RoomGone exception round trip.
// ---------------------------------------------------------------------------

// Echoes messages back prefixed until one says "gone", then fails modeled.
class EchoHandler final : public pipe::PipeHandler {
 public:
  smithy::Outcome<smithy::Unit> Exchange(const pipe::ExchangeInput&,
                                         pipe::ExchangeServerStream& stream,
                                         const smithy::server::RequestContext&) override {
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) return smithy::Unit{};
      if (!(**event).is_message()) continue;
      const std::string& text = (**event).as_message().text;
      if (text == "gone") {
        smithy::Error gone = smithy::Error::Modeled("RoomGone", "the room left");
        gone.set_detail(pipe::RoomGone{.message = "the room left"});
        return gone;
      }
      pipe::ChatMessage echo;
      echo.text = "echo:" + text;
      if (!stream.Send(pipe::ServerEvents::FromMessage(echo)).ok()) return smithy::Unit{};
    }
  }

  smithy::Outcome<smithy::Unit> Watch(const pipe::WatchInput&, pipe::WatchServerStream& stream,
                                      const smithy::server::RequestContext&) override {
    pipe::ChatMessage message;
    message.text = "pushed";
    (void)stream.Send(pipe::ServerEvents::FromMessage(message));
    return smithy::Unit{};
  }
};

class CborStreamEndToEndTest : public testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<pipe::PipeServer>(std::make_shared<EchoHandler>());
    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = std::make_shared<smithy::http::Loopback>();
    config.websocket_dialer = [this](const smithy::http::WebSocketDialRequest& request)
        -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
      dialed_target_ = request.target;
      auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
      smithy::http::HttpRequest upgrade;
      upgrade.method = "GET";
      upgrade.target = request.target;
      upgrade.headers = request.headers;
      sessions_.push_back(far);
      threads_.emplace_back([serve = server_->StreamRouter()->Serve(), upgrade, session = far] {
        serve(upgrade, *session);
      });
      return near;
    };
    auto client = pipe::PipeClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<pipe::PipeClient>(std::move(*client));
  }

  void TearDown() override {
    for (auto& session : sessions_) session->Close();
    for (std::thread& thread : threads_) thread.join();
  }

  std::unique_ptr<pipe::PipeServer> server_;
  std::unique_ptr<pipe::PipeClient> client_;
  std::vector<std::shared_ptr<smithy::http::WebSocket>> sessions_;
  std::vector<std::thread> threads_;
  std::string dialed_target_;
};

TEST_F(CborStreamEndToEndTest, BidiCborEventsRoundTripThroughTheGeneratedServer) {
  auto stream = client_->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(dialed_target_, "/service/Pipe/operation/Exchange");

  pipe::ChatMessage message;
  message.text = "hello";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto echo = stream->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value()) << (echo.ok() ? "closed" : echo.error().message());
  ASSERT_TRUE((**echo).is_message());
  EXPECT_EQ((**echo).as_message().text, "echo:hello");

  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
}

TEST_F(CborStreamEndToEndTest, RoomGoneRoundTripsAsATypedMidStreamError) {
  auto stream = client_->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  pipe::ChatMessage message;
  message.text = "gone";
  ASSERT_TRUE(stream->Send(pipe::ClientEvents::FromMessage(message)).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "RoomGone");
  EXPECT_EQ(outcome.error().message(), "the room left");
  const pipe::RoomGone* detail = outcome.error().detail<pipe::RoomGone>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->message.value_or(""), "the room left");
}

TEST_F(CborStreamEndToEndTest, WatchPushesOverTheFixedUri) {
  auto stream = client_->Watch();
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  EXPECT_EQ(dialed_target_, "/service/Pipe/operation/Watch");
  auto update = stream->Receive();
  ASSERT_TRUE(update.ok() && update->has_value());
  ASSERT_TRUE((**update).is_message());
  EXPECT_EQ((**update).as_message().text, "pushed");
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
}

}  // namespace
