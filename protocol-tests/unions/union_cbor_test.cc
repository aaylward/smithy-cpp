// Union x rpcv2Cbor conformance (issue #48): until this suite, union
// round-tripping was only pinned for simpleRestJson — the cbor cell relied on
// the seeded random integration tests, which flip a coin on whether the union
// appears at all and can only prove serde self-consistency, not wire
// correctness. These tests pin the wire subdocument for every SinkChoice
// variant deterministically, in all four directions: client encode, client
// decode, server decode, server encode — plus the reject cells (empty,
// multi-member, unknown-member, null-member unions) and the __type
// discriminator tolerance on both sides.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "example/roundtrip/rpc/client.h"
#include "example/roundtrip/rpc/server.h"
#include "smithy/cbor/cbor.h"
#include "smithy/client/config.h"
#include "smithy/core/document.h"
#include "smithy/testing/protocol_test.h"

namespace example::roundtrip::rpc {
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

// A wire body for PutSinkRpc carrying only the members the union cell needs.
std::string BodyWithChoice(const smithy::Document& choice) {
  smithy::DocumentMap sink;
  sink.emplace("name", smithy::Document("n"));
  sink.emplace("choice", choice);
  smithy::DocumentMap body;
  body.emplace("sinkId", smithy::Document("s1"));
  body.emplace("sink", smithy::Document(std::move(sink)));
  return smithy::cbor::Encode(smithy::Document(std::move(body))).ToString();
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

// The choice subdocument of a captured PutSinkRpc request body.
smithy::Document ChoiceOf(const std::string& wire_body) {
  auto doc = smithy::cbor::Decode(smithy::Blob::FromString(wire_body));
  EXPECT_TRUE(doc.ok());
  if (!doc.ok()) return smithy::Document(nullptr);
  const smithy::Document* sink = doc->Find("sink");
  EXPECT_NE(sink, nullptr);
  if (sink == nullptr) return smithy::Document(nullptr);
  const smithy::Document* choice = sink->Find("choice");
  EXPECT_NE(choice, nullptr);
  return choice == nullptr ? smithy::Document(nullptr) : *choice;
}

class UnionCborClientTest : public testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_shared<smithy::testing::CapturingTransport>();
    smithy::DocumentMap ok_body;
    ok_body.emplace("sinkId", smithy::Document("s1"));
    transport_->next_response = smithy::http::HttpResponse{
        200, {}, smithy::cbor::Encode(smithy::Document(std::move(ok_body))).ToString()};
    smithy::ClientConfig config;
    config.retry.max_attempts = 1;
    config.http_client = transport_;
    auto client = RoundTripRpcClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<RoundTripRpcClient>(std::move(*client));
  }

  std::shared_ptr<smithy::testing::CapturingTransport> transport_;
  std::unique_ptr<RoundTripRpcClient> client_;
};

TEST_F(UnionCborClientTest, EncodesEachVariantAsASingleMemberMap) {
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
    const smithy::Document choice = ChoiceOf(transport_->last_request.body);
    ASSERT_TRUE(choice.is_map());
    EXPECT_EQ(choice.as_map().size(), 1u) << "a union must serialize exactly one member";
    EXPECT_EQ(choice, cell.expected);
    EXPECT_EQ(transport_->last_request.headers.Get("smithy-protocol"), "rpc-v2-cbor");
  }
}

TEST_F(UnionCborClientTest, DecodesEachVariantFromAResponse) {
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
    transport_->next_response = smithy::http::HttpResponse{200, {}, BodyWithChoice(cell.wire)};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_TRUE(outcome.ok()) << outcome.error().message();
    ASSERT_TRUE(outcome->sink.has_value());
    ASSERT_TRUE(outcome->sink->choice.has_value());
    EXPECT_EQ(*outcome->sink->choice, cell.expected);
  }
}

TEST_F(UnionCborClientTest, RejectsInvalidUnionsInResponses) {
  // Each cell pins its diagnosis, not just the rejection: a union declined
  // for the wrong reason (a generic parse failure, a missing-field error)
  // would mask the exactly-one-member rule this test defends.
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
      {[] {
         smithy::DocumentMap map;
         map.emplace("text", smithy::Document(nullptr));
         return smithy::Document(std::move(map));
       }(),
       "null member", "unknown or missing union member"},
      {smithy::Document("not a map"), "non-map union", "expected a map on the wire"},
  };
  for (const auto& cell : cells) {
    transport_->next_response = smithy::http::HttpResponse{200, {}, BodyWithChoice(cell.wire)};
    const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
    ASSERT_FALSE(outcome.ok()) << cell.why;
    EXPECT_NE(outcome.error().message().find(cell.diagnosis), std::string::npos)
        << cell.why << ": " << outcome.error().message();
  }
}

TEST_F(UnionCborClientTest, ToleratesATypeDiscriminatorNextToTheMember) {
  smithy::DocumentMap map;
  map.emplace("__type", smithy::Document("example.roundtrip#SinkChoice"));
  map.emplace("text", smithy::Document("discriminated"));
  transport_->next_response =
      smithy::http::HttpResponse{200, {}, BodyWithChoice(smithy::Document(std::move(map)))};
  const auto outcome = client_->PutSinkRpc(InputWithChoice(SinkChoice::FromCount(0)));
  ASSERT_TRUE(outcome.ok()) << outcome.error().message();
  EXPECT_EQ(*outcome->sink->choice, SinkChoice::FromText("discriminated"));
}

// --- Server side: the same cells through the generated request path -------

class RecordingHandler : public RoundTripRpcHandler {
 public:
  smithy::Outcome<PutSinkRpcOutput> PutSinkRpc(const PutSinkRpcInput& input) override {
    last = input;
    PutSinkRpcOutput output;
    output.sinkId = input.sinkId;
    output.sink = input.sink;  // echo, so the response leg is exercised too
    return output;
  }
  std::optional<PutSinkRpcInput> last;
};

class UnionCborServerTest : public testing::Test {
 protected:
  smithy::http::HttpResponse Send(const std::string& body) {
    smithy::http::HttpRequest request;
    request.method = "POST";
    request.target = "/service/RoundTripRpc/operation/PutSinkRpc";
    request.headers.Set("smithy-protocol", "rpc-v2-cbor");
    request.headers.Set("content-type", "application/cbor");
    request.body = body;
    return server_.Handler()(request);
  }

  std::shared_ptr<RecordingHandler> handler_ = std::make_shared<RecordingHandler>();
  RoundTripRpcServer server_{handler_};
};

TEST_F(UnionCborServerTest, DecodesEachVariantAndEchoesItBack) {
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
    const auto response = Send(BodyWithChoice(cell.wire));
    ASSERT_EQ(response.status, 200) << response.body;
    ASSERT_TRUE(handler_->last.has_value());
    ASSERT_TRUE(handler_->last->sink.has_value() && handler_->last->sink->choice.has_value());
    EXPECT_EQ(*handler_->last->sink->choice, cell.expected);

    // The echoed response body carries the identical union subdocument.
    EXPECT_EQ(ChoiceOf(response.body), cell.wire);
    handler_->last.reset();
  }
}

TEST_F(UnionCborServerTest, RejectsInvalidUnionsBeforeTheHandler) {
  const struct {
    smithy::Document wire;
    const char* diagnosis;
  } cells[] = {
      {smithy::Document(smithy::DocumentMap{}), "expected exactly one union member"},
      {[] {
         smithy::DocumentMap map;
         map.emplace("text", smithy::Document("a"));
         map.emplace("count", smithy::Document(std::int64_t{1}));
         return smithy::Document(std::move(map));
       }(),
       "expected exactly one union member"},
      {[] {
         smithy::DocumentMap map;
         map.emplace("futureMember", smithy::Document(std::int64_t{1}));
         return smithy::Document(std::move(map));
       }(),
       "unknown or missing union member"},
  };
  for (const auto& cell : cells) {
    const auto response = Send(BodyWithChoice(cell.wire));
    EXPECT_EQ(response.status, 400) << response.body;
    EXPECT_FALSE(handler_->last.has_value());
    // The 400's error body names the union rule that was violated.
    auto body = smithy::cbor::Decode(smithy::Blob::FromString(response.body));
    ASSERT_TRUE(body.ok());
    const smithy::Document* message = body->Find("message");
    ASSERT_NE(message, nullptr);
    EXPECT_NE(message->as_string().find(cell.diagnosis), std::string::npos) << message->as_string();
  }
}

}  // namespace
}  // namespace example::roundtrip::rpc
