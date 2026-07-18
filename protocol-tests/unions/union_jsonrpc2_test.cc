// Union x jsonRpc2 conformance (issue #48): the second untested union cell.
// Same shape as union_cbor_test.cc — every SinkChoice variant pinned in all
// four directions plus the reject cells — but through the JSON-RPC 2.0
// envelope, whose params/result nesting is exactly where a protocol
// generator could diverge from the shared serde.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/roundtrip/jsonrpc/client.h"
#include "example/roundtrip/jsonrpc/server.h"
#include "smithy/client/config.h"
#include "smithy/core/document.h"
#include "smithy/json/json.h"
#include "smithy/testing/protocol_test.h"

namespace example::roundtrip::jsonrpc {
namespace {

smithy::Document TextChoiceDoc(const std::string& text) {
  smithy::DocumentMap map;
  map.emplace("text", smithy::Document(text));
  return smithy::Document(std::move(map));
}

smithy::Document CountChoiceDoc(std::int64_t count) {
  smithy::DocumentMap map;
  map.emplace("count", smithy::Document(count));
  return smithy::Document(std::move(map));
}

smithy::Document NestedChoiceDoc(const std::string& label, std::int64_t depth) {
  smithy::DocumentMap nested;
  nested.emplace("label", smithy::Document(label));
  nested.emplace("depth", smithy::Document(depth));
  smithy::DocumentMap map;
  map.emplace("nested", smithy::Document(std::move(nested)));
  return smithy::Document(std::move(map));
}

smithy::Document PayloadWithChoice(const smithy::Document& choice) {
  smithy::DocumentMap sink;
  sink.emplace("name", smithy::Document("n"));
  sink.emplace("choice", choice);
  smithy::DocumentMap payload;
  payload.emplace("sinkId", smithy::Document("s1"));
  payload.emplace("sink", smithy::Document(std::move(sink)));
  return smithy::Document(std::move(payload));
}

// A success envelope answering the client's fixed request id 1.
std::string ResultEnvelope(const smithy::Document& result) {
  smithy::DocumentMap envelope;
  envelope.emplace("jsonrpc", smithy::Document("2.0"));
  envelope.emplace("id", smithy::Document(std::int64_t{1}));
  envelope.emplace("result", result);
  return smithy::json::Encode(smithy::Document(std::move(envelope)));
}

PutSinkRpcInput InputWithChoice(SinkChoice choice) {
  KitchenSink sink;
  sink.name = "n";
  sink.choice = std::move(choice);
  PutSinkRpcInput input;
  input.sinkId = "s1";
  input.sink = std::move(sink);
  return input;
}

// The params.sink.choice subdocument of a captured JSON-RPC request.
smithy::Document RequestChoiceOf(const std::string& wire_body) {
  auto doc = smithy::json::Decode(wire_body);
  EXPECT_TRUE(doc.ok());
  if (!doc.ok()) return smithy::Document(nullptr);
  const smithy::Document* params = doc->Find("params");
  EXPECT_NE(params, nullptr) << wire_body;
  if (params == nullptr) return smithy::Document(nullptr);
  const smithy::Document* sink = params->Find("sink");
  EXPECT_NE(sink, nullptr) << wire_body;
  if (sink == nullptr) return smithy::Document(nullptr);
  const smithy::Document* choice = sink->Find("choice");
  EXPECT_NE(choice, nullptr) << wire_body;
  return choice == nullptr ? smithy::Document(nullptr) : *choice;
}

class UnionJsonRpc2ClientTest : public testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_shared<smithy::testing::CapturingTransport>();
    smithy::DocumentMap ok_result;
    ok_result.emplace("sinkId", smithy::Document("s1"));
    transport_->next_response =
        smithy::http::HttpResponse{200, {}, ResultEnvelope(smithy::Document(std::move(ok_result)))};
    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = transport_;
    auto client = RoundTripJsonRpcClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<RoundTripJsonRpcClient>(std::move(*client));
  }

  std::shared_ptr<smithy::testing::CapturingTransport> transport_;
  std::unique_ptr<RoundTripJsonRpcClient> client_;
};

TEST_F(UnionJsonRpc2ClientTest, EncodesEachVariantInsideTheEnvelopeParams) {
  const struct {
    SinkChoice choice;
    smithy::Document expected;
  } cells[] = {
      {SinkChoice::FromText("wire text"), TextChoiceDoc("wire text")},
      {SinkChoice::FromCount(-7), CountChoiceDoc(-7)},
      {SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "L";
         nested.depth = 3;
         return nested;
       }()),
       NestedChoiceDoc("L", 3)},
  };
  for (const auto& cell : cells) {
    ASSERT_TRUE(client_->PutSinkRpc(InputWithChoice(cell.choice)).ok());
    const smithy::Document choice = RequestChoiceOf(transport_->last_request.body);
    ASSERT_TRUE(choice.is_map());
    EXPECT_EQ(choice.as_map().size(), 1u) << "a union must serialize exactly one member";
    EXPECT_EQ(choice, cell.expected);
  }
}

TEST_F(UnionJsonRpc2ClientTest, DecodesEachVariantFromAResultEnvelope) {
  const struct {
    smithy::Document wire;
    SinkChoice expected;
  } cells[] = {
      {TextChoiceDoc("from server"), SinkChoice::FromText("from server")},
      {CountChoiceDoc(42), SinkChoice::FromCount(42)},
      {NestedChoiceDoc("deep", 9), SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "deep";
         nested.depth = 9;
         return nested;
       }())},
  };
  for (const auto& cell : cells) {
    transport_->next_response =
        smithy::http::HttpResponse{200, {}, ResultEnvelope(PayloadWithChoice(cell.wire))};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_TRUE(outcome.ok()) << outcome.error().message();
    ASSERT_TRUE(outcome->sink.has_value());
    ASSERT_TRUE(outcome->sink->choice.has_value());
    EXPECT_EQ(*outcome->sink->choice, cell.expected);
  }
}

TEST_F(UnionJsonRpc2ClientTest, RejectsInvalidUnionsInResultEnvelopes) {
  // Each cell pins its diagnosis, not just the rejection: a union declined
  // for the wrong reason would mask the exactly-one-member rule.
  const struct {
    smithy::Document wire;
    const char* why;
    const char* diagnosis;
  } cells[] = {
      {smithy::Document(smithy::DocumentMap{}), "empty union", "expected exactly one union member"},
      {[] {
         smithy::DocumentMap map;
         map.emplace("text", smithy::Document("a"));
         map.emplace("count", smithy::Document(std::int64_t{1}));
         return smithy::Document(std::move(map));
       }(),
       "two members set", "expected exactly one union member"},
      {[] {
         smithy::DocumentMap map;
         map.emplace("futureMember", smithy::Document(std::int64_t{1}));
         return smithy::Document(std::move(map));
       }(),
       "unknown member", "unknown or missing union member"},
      {smithy::Document("not a map"), "non-map union", "expected a map on the wire"},
  };
  for (const auto& cell : cells) {
    transport_->next_response =
        smithy::http::HttpResponse{200, {}, ResultEnvelope(PayloadWithChoice(cell.wire))};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_FALSE(outcome.ok()) << cell.why;
    EXPECT_NE(outcome.error().message().find(cell.diagnosis), std::string::npos)
        << cell.why << ": " << outcome.error().message();
  }
}

// --- Server side ----------------------------------------------------------

class RecordingHandler : public RoundTripJsonRpcHandler {
 public:
  smithy::Outcome<PutSinkRpcOutput> PutSinkRpc(const PutSinkRpcInput& input,
                                               const smithy::server::RequestContext&) override {
    last = input;
    PutSinkRpcOutput output;
    output.sinkId = input.sinkId;
    output.sink = input.sink;
    return output;
  }
  std::optional<PutSinkRpcInput> last;
};

class UnionJsonRpc2ServerTest : public testing::Test {
 protected:
  smithy::http::HttpResponse Send(const smithy::Document& params) {
    smithy::DocumentMap envelope;
    envelope.emplace("jsonrpc", smithy::Document("2.0"));
    envelope.emplace("method", smithy::Document("PutSinkRpc"));
    envelope.emplace("id", smithy::Document(std::int64_t{7}));
    envelope.emplace("params", params);
    smithy::http::HttpRequest request;
    request.method = "POST";
    request.target = "/";
    request.headers.Set("content-type", "application/json");
    request.body = smithy::json::Encode(smithy::Document(std::move(envelope)));
    return server_.Handler()(request);
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RoundTripJsonRpcServer server_{handler_};
};

TEST_F(UnionJsonRpc2ServerTest, DecodesEachVariantAndEchoesItBack) {
  const struct {
    smithy::Document wire;
    SinkChoice expected;
  } cells[] = {
      {TextChoiceDoc("to server"), SinkChoice::FromText("to server")},
      {CountChoiceDoc(-1), SinkChoice::FromCount(-1)},
      {NestedChoiceDoc("srv", 2), SinkChoice::FromNested([] {
         NestedConfig nested;
         nested.label = "srv";
         nested.depth = 2;
         return nested;
       }())},
  };
  for (const auto& cell : cells) {
    const auto response = Send(PayloadWithChoice(cell.wire));
    ASSERT_EQ(response.status, 200) << response.body;
    ASSERT_TRUE(handler_->last.has_value());
    ASSERT_TRUE(handler_->last->sink.has_value() && handler_->last->sink->choice.has_value());
    EXPECT_EQ(*handler_->last->sink->choice, cell.expected);

    // The echoed result carries the identical union subdocument, under the
    // envelope's result member and answering the request id.
    auto body = smithy::json::Decode(response.body);
    ASSERT_TRUE(body.ok()) << response.body;
    EXPECT_EQ(body->Find("id")->as_int(), 7);
    const smithy::Document* result = body->Find("result");
    ASSERT_NE(result, nullptr) << response.body;
    EXPECT_EQ(*result->Find("sink")->Find("choice"), cell.wire);
    handler_->last.reset();
  }
}

TEST_F(UnionJsonRpc2ServerTest, RejectsInvalidUnionsBeforeTheHandler) {
  const smithy::Document invalid[] = {
      smithy::Document(smithy::DocumentMap{}),
      [] {
        smithy::DocumentMap map;
        map.emplace("text", smithy::Document("a"));
        map.emplace("count", smithy::Document(std::int64_t{1}));
        return smithy::Document(std::move(map));
      }(),
      [] {
        smithy::DocumentMap map;
        map.emplace("futureMember", smithy::Document(std::int64_t{1}));
        return smithy::Document(std::move(map));
      }(),
  };
  for (const auto& wire : invalid) {
    const auto response = Send(PayloadWithChoice(wire));
    // jsonRpc2 answers protocol-layer failures as an error envelope on 200.
    EXPECT_EQ(response.status, 200) << response.body;
    auto body = smithy::json::Decode(response.body);
    ASSERT_TRUE(body.ok()) << response.body;
    const smithy::Document* error = body->Find("error");
    ASSERT_NE(error, nullptr) << response.body;
    // The envelope names the union rule that was violated, so a rejection
    // for the wrong reason cannot hide behind the error member's presence.
    EXPECT_NE(response.body.find("union member"), std::string::npos) << response.body;
    EXPECT_FALSE(handler_->last.has_value());
  }
}

}  // namespace
}  // namespace example::roundtrip::jsonrpc
