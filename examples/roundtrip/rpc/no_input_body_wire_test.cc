// Wire-level pin for issue #68: the rpcv2Cbor server ignores request bodies
// sent to a no-input operation. Conforming clients never send one, so no
// upstream conformance case adjudicates this — but #67 fixed a client/server
// asymmetry exactly here (servers used to decode-and-400 what clients would
// never produce). The contrast case pins that operations WITH modeled input
// still reject the same garbage, so the leniency is no-input-specific.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "example/roundtrip/rpc/server.h"
#include "smithy/http/transport.h"
#include "smithy/testing/protocol_test.h"

namespace example::roundtrip::rpc {
namespace {

class RecordingHandler final : public RoundTripRpcHandler {
 public:
  smithy::Outcome<PutSinkRpcOutput> PutSinkRpc(const PutSinkRpcInput& input) override {
    put_sink_calls++;
    return PutSinkRpcOutput{.sinkId = input.sinkId};
  }

  smithy::Outcome<PingOutput> Ping(const PingInput&) override {
    ping_calls++;
    return PingOutput{};
  }

  int ping_calls = 0;
  int put_sink_calls = 0;
};

smithy::http::HttpRequest CborRequest(const std::string& operation, std::string body) {
  return smithy::testing::Rpcv2CborRequest("RoundTripRpc", operation, std::move(body));
}

TEST(NoInputBodyTest, GarbageBodyAtANoInputOperationIsIgnored) {
  auto handler = std::make_shared<RecordingHandler>();
  RoundTripRpcServer server(handler);
  auto response = server.Handler()(CborRequest("Ping", "\xff\xfenot cbor garbage"));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(handler->ping_calls, 1);
}

TEST(NoInputBodyTest, EmptyBodyAtANoInputOperationSucceedsToo) {
  auto handler = std::make_shared<RecordingHandler>();
  RoundTripRpcServer server(handler);
  auto response = server.Handler()(CborRequest("Ping", ""));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(handler->ping_calls, 1);
}

TEST(NoInputBodyTest, TheSameGarbageAtAModeledInputOperationIsRejected) {
  auto handler = std::make_shared<RecordingHandler>();
  RoundTripRpcServer server(handler);
  auto response = server.Handler()(CborRequest("PutSinkRpc", "\xff\xfenot cbor garbage"));
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(handler->put_sink_calls, 0);
}

}  // namespace
}  // namespace example::roundtrip::rpc
