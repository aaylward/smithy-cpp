// The jsonRpc2 stream wire over REAL WebSockets (ADR-0023; issue #123's
// acceptance): BeastServerTransport in raw-text mode upgrades on the
// generated StreamRouter — both serve seams — the generated client dials
// the shared endpoint through the default Beast dialer (no injection, TLS
// included), and a browser-shaped raw-text peer exchanges the pinned
// envelope text on the wire itself. The in-memory half of the same flows
// lives in stream_e2e_test.cc.
//
// CI-only like chat_e2e_beast_test: Boost doesn't fetch behind the
// documented download-blocking proxy (docs/development.md).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "accumulate_handler.h"
#include "example/calculator/client.h"
#include "example/calculator/server.h"
#include "smithy/client/config.h"
#include "smithy/core/error.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/tls_test_identity.h"

namespace example::calculator {
namespace {

smithy::eventstream::Message RawText(std::string text) {
  smithy::eventstream::Message message;
  message.payload = smithy::Blob::FromString(std::move(text));
  return message;
}

class AccumulateBeastEndToEndTest : public testing::Test {
 protected:
  // The production wiring (ADR-0023): mount the generated StreamRouter and
  // flip the transport to the raw-text wire — the one flag a jsonRpc2
  // streaming server needs beyond the ADR-0016 two-liner.
  void Start(bool session_seam, bool tls = false) {
    server_ = session_seam
                  ? std::make_unique<CalculatorServer>(
                        std::make_shared<AsyncAccumulatingCalculator>())
                  : std::make_unique<CalculatorServer>(std::make_shared<AccumulatingCalculator>());
    smithy::http::BeastServerTransport::Options options;
    options.websocket_gate = server_->StreamRouter()->Gate();
    if (session_seam) {
      options.on_websocket_session = server_->StreamRouter()->ServeSession();
    } else {
      options.on_websocket = server_->StreamRouter()->Serve();
    }
    options.websocket_raw_text_frames = true;
    if (tls) {
      options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
      options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
    }
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(options);
    ASSERT_TRUE(transport_->Start(server_->Handler()).ok());

    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    if (tls) {
      // One endpoint configures both directions: the unary transport and
      // the streaming dial derive wss/TLS from it (nothing twice).
      config.endpoint = "https://127.0.0.1:" + std::to_string(transport_->port());
      config.tls.ca_pem = smithy::testing::kTestCertificatePem;
      auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
      ASSERT_TRUE(http_client.ok()) << http_client.error().message();
      config.http_client = *http_client;
    } else {
      config.endpoint = "http://127.0.0.1:" + std::to_string(transport_->port());
    }
    auto client = CalculatorClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<CalculatorClient>(std::move(*client));
  }

  void TearDown() override {
    if (transport_ != nullptr) transport_->Stop();
  }

  void RunningTotalsRoundTrip() {
    AccumulateInput input;
    input.start = 10.0;
    auto stream = client_->Accumulate(input);
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 5})).ok());
    auto first = stream->Receive();
    ASSERT_TRUE(first.ok() && first->has_value());
    EXPECT_EQ((**first).as_total().value, 15.0);

    ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 0})).ok());
    auto end = stream->Receive();
    ASSERT_TRUE(end.ok()) << end.error().message();
    EXPECT_FALSE(end->has_value());  // the terminal result, then the close
  }

  std::unique_ptr<CalculatorServer> server_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
  std::unique_ptr<CalculatorClient> client_;
};

TEST_F(AccumulateBeastEndToEndTest, RunningTotalsRoundTripOverRealWebSockets) {
  Start(/*session_seam=*/false);
  RunningTotalsRoundTrip();
}

TEST_F(AccumulateBeastEndToEndTest, TheSessionSeamServesTheSameWire) {
  Start(/*session_seam=*/true);
  RunningTotalsRoundTrip();
}

TEST_F(AccumulateBeastEndToEndTest, TheModeledOverflowIsTypedOverTheRealWire) {
  Start(/*session_seam=*/true);
  AccumulateInput input;
  input.start = 90.0;
  auto stream = client_->Accumulate(input);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  ASSERT_TRUE(stream->Send(Terms::FromAdd(Term{.value = 20})).ok());
  auto outcome = stream->Receive();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), smithy::ErrorKind::kModeled);
  EXPECT_EQ(outcome.error().code(), "Overflow");
  const Overflow* detail = outcome.error().detail<Overflow>();
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->limit, kAccumulateLimit);
}

TEST_F(AccumulateBeastEndToEndTest, UnaryAndStreamingShareOnePort) {
  Start(/*session_seam=*/false);
  auto sum = client_->Add(AddInput{.a = 2, .b = 3});
  ASSERT_TRUE(sum.ok()) << sum.error().message();
  EXPECT_EQ(sum->sum, 5.0);
  RunningTotalsRoundTrip();
}

TEST_F(AccumulateBeastEndToEndTest, TlsStreamsCarryTheEnvelopes) {
  Start(/*session_seam=*/true, /*tls=*/true);
  auto sum = client_->Add(AddInput{.a = 1, .b = 1});  // unary over https
  ASSERT_TRUE(sum.ok()) << sum.error().message();
  RunningTotalsRoundTrip();  // streaming over wss
}

TEST_F(AccumulateBeastEndToEndTest, ABrowserShapedPeerExchangesThePinnedText) {
  // What a page does — new WebSocket(url), send strings, JSON.parse what
  // arrives — expressed through the raw-text dial: every frame below is
  // one text WebSocket message carrying exactly these bytes.
  Start(/*session_seam=*/true);
  auto peer = smithy::http::BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = transport_->port(), .raw_text_frames = true});
  ASSERT_TRUE(peer.ok()) << peer.error().message();

  ASSERT_TRUE(
      (*peer)
          ->Send(RawText(R"({"jsonrpc":"2.0","method":"Accumulate","params":{"start":1},"id":7})"))
          .ok());
  ASSERT_TRUE(
      (*peer)
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"add","params":{"id":7,"payload":{"value":2}}})"))
          .ok());
  auto total = (*peer)->Receive();
  ASSERT_TRUE(total.ok() && total->has_value());
  EXPECT_EQ((**total).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"total","params":{"id":7,"payload":{"value":3.0}}})");

  ASSERT_TRUE(
      (*peer)
          ->Send(RawText(
              R"({"jsonrpc":"2.0","method":"add","params":{"id":7,"payload":{"value":0}}})"))
          .ok());
  auto terminal = (*peer)->Receive();
  ASSERT_TRUE(terminal.ok() && terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(), R"({"id":7,"jsonrpc":"2.0","result":{}})");
  auto end = (*peer)->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());  // a vanilla peer saw one call, one response
}

}  // namespace
}  // namespace example::calculator
