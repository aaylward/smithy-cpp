// The streaming client halves of the compile harness (ADR-0016). A separate
// target from gauntlet_compile_test because generated streaming clients fall
// back to smithy::http::BeastWebSocketClient::Dialer(), so linking them pulls
// in Boost — this test joins the Beast targets behind the documented local
// exclusion list (docs/development.md); CI runs it. The injected in-memory
// dialer below is the ClientConfig::websocket_dialer seam working as designed:
// streams without a wire, exactly how consumer tests run them.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "compile/streaming/cbor/client.h"
#include "compile/streaming/rest/client.h"
#include "smithy/client/config.h"
#include "smithy/http/websocket_pair.h"

namespace {

// A dialer handing out one end of an in-memory pair, keeping the other for
// the test to play the server.
smithy::ClientConfig InMemoryConfig(std::shared_ptr<smithy::http::WebSocket>* server_end,
                                    std::string* dialed_target) {
  smithy::ClientConfig config;
  config.endpoint = "http://localhost:8080";
  config.websocket_dialer = [server_end,
                             dialed_target](const smithy::http::WebSocketDialRequest& request)
      -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
    *dialed_target = request.target;
    auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
    *server_end = std::move(far);
    return near;
  };
  return config;
}

TEST(StreamingCompileTest, RestClientOpensAServerPushStream) {
  std::shared_ptr<smithy::http::WebSocket> server_end;
  std::string dialed_target;
  auto client =
      compile::streaming::rest::RelayClient::Create(InMemoryConfig(&server_end, &dialed_target));
  ASSERT_TRUE(client.ok());

  auto stream = client->Watch();
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed_target, "/watch");

  server_end->Close();
  auto received = stream->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close
}

TEST(StreamingCompileTest, CborClientExchangesOnTheFixedUpgradeUri) {
  std::shared_ptr<smithy::http::WebSocket> server_end;
  std::string dialed_target;
  auto client =
      compile::streaming::cbor::PipeClient::Create(InMemoryConfig(&server_end, &dialed_target));
  ASSERT_TRUE(client.ok());

  auto stream = client->Exchange({});
  ASSERT_TRUE(stream.ok());
  EXPECT_EQ(dialed_target, "/service/Pipe/operation/Exchange");

  compile::streaming::cbor::ChatMessage message;
  message.text = "hello";
  ASSERT_TRUE(stream->Send(compile::streaming::cbor::ClientEvents::FromMessage(message)).ok());
  auto frame = server_end->Receive();
  ASSERT_TRUE(frame.ok());
  ASSERT_TRUE(frame->has_value());
  stream->Close();
}

}  // namespace
