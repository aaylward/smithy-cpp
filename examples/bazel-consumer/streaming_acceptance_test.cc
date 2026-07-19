// Out-of-tree acceptance for the generated streaming API (ADR-0016, slice
// 3's consumer e2e): a consumer's generated client runs a bidirectional
// round trip against its generated server's StreamRouter through the module
// boundary, the streams riding an injected InMemoryWebSocketPair dialer —
// the documented Boost-free way to test streams (no wire, no sockets; the
// serve callback is invoked directly with the synthesized upgrade request a
// real transport would deliver). websocket_acceptance_test covers the
// real-wire transport underneath.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "acme/chat/client.h"
#include "acme/chat/server.h"
#include "smithy/client/config.h"
#include "smithy/http/message.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace {

// Echoes every note back with an "echo:" prefix until the client closes.
class EchoHandler final : public acme::chat::ChatHandler {
 public:
  smithy::Outcome<smithy::Unit> Exchange(
      const acme::chat::ExchangeInput&,
      smithy::eventstream::EventStream<acme::chat::Notes, acme::chat::Notes>& stream,
      const smithy::server::RequestContext&) override {
    while (true) {
      auto event = stream.Receive();
      if (!event.ok() || !event->has_value()) return smithy::Unit{};
      acme::chat::Note reply;
      reply.text = "echo:" + (**event).as_note().text;
      if (!stream.Send(acme::chat::Notes::FromNote(reply)).ok()) return smithy::Unit{};
    }
  }
};

class StreamingAcceptanceTest : public testing::Test {
 protected:
  void TearDown() override {
    // Close is idempotent; this unblocks the serve loop if a failed test
    // body left it mid-Receive, so the join cannot hang.
    if (server_session_ != nullptr) server_session_->Close();
    if (serve_thread_.joinable()) serve_thread_.join();
  }

  acme::chat::ChatServer server_{std::make_shared<EchoHandler>()};
  std::shared_ptr<smithy::http::WebSocket> server_session_;
  std::thread serve_thread_;
};

TEST_F(StreamingAcceptanceTest, ABidiRoundTripCrossesTheModuleBoundary) {
  smithy::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [this](const smithy::http::WebSocketDialRequest& request)
      -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
    auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
    smithy::http::HttpRequest upgrade;
    upgrade.method = "GET";
    upgrade.target = request.target;
    upgrade.headers = request.headers;
    server_session_ = far;
    serve_thread_ = std::thread([serve = server_.StreamRouter()->Serve(), upgrade, session = far] {
      serve(upgrade, *session);
    });
    return near;
  };
  auto client = acme::chat::ChatClient::Create(std::move(config));
  ASSERT_TRUE(client.ok()) << client.error().message();

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  for (int i = 0; i < 3; ++i) {
    acme::chat::Note note;
    note.text = "note-" + std::to_string(i);
    ASSERT_TRUE(stream->Send(acme::chat::Notes::FromNote(note)).ok());
    auto echo = stream->Receive();
    ASSERT_TRUE(echo.ok() && echo->has_value());
    ASSERT_TRUE((**echo).is_note());
    EXPECT_EQ((**echo).as_note().text, "echo:note-" + std::to_string(i));
  }

  stream->Close();
  auto end = stream->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());  // the server's acknowledging clean close
}

}  // namespace
