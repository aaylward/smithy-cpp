// Pins ADR-0023's JSON-RPC stream wire: the exact notification text the
// encoder mints (byte-pinned — text frames ARE the wire), the round trip
// back to the Message the binary wire would carry, the terminal-response
// classification (result = the clean end, error = the exception message),
// and the fail-closed bank — every malformed envelope must surface as
// Error::Serialization, never as a half-understood message.

#include "smithy/eventstream/jsonrpc_frame.h"

#include <gtest/gtest.h>

#include <string>

#include "smithy/core/document.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/frame.h"

namespace smithy::eventstream {
namespace {

const Document kId(1);

std::string EncodeOrDie(const Message& message, const Document& id = kId) {
  auto text = EncodeJsonRpcNotification(message, id);
  EXPECT_TRUE(text.ok()) << text.error().message();
  return text.ok() ? *text : std::string();
}

JsonRpcStreamFrame DecodeOrDie(std::string_view text, const Document& id = kId) {
  auto frame = DecodeJsonRpcStreamFrame(text, id);
  EXPECT_TRUE(frame.ok()) << frame.error().message();
  return frame.ok() ? *std::move(frame) : JsonRpcStreamFrame{};
}

void ExpectEncodeRefusal(const Message& message, const char* why) {
  const auto text = EncodeJsonRpcNotification(message, kId);
  ASSERT_FALSE(text.ok()) << why;
  EXPECT_EQ(text.error().kind(), ErrorKind::kValidation) << why;
}

void ExpectDecodeRefusal(std::string_view text, const char* why, const Document& id = kId) {
  const auto frame = DecodeJsonRpcStreamFrame(text, id);
  ASSERT_FALSE(frame.ok()) << why;
  EXPECT_EQ(frame.error().kind(), ErrorKind::kSerialization) << why;
}

TEST(JsonRpcFrameTest, EventsRenderThePinnedNotificationText) {
  // Byte-pinned: this text is what a browser's onmessage receives. The
  // codec's JSON output is deterministic (sorted keys, compact), so
  // string equality is wire equality. The id echo rides inside params —
  // the eth_subscribe shape (ADR-0023).
  const std::string text = EncodeOrDie(
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})")));
  EXPECT_EQ(text,
            R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"text":"hi"}}})");
}

TEST(JsonRpcFrameTest, AnAbsentContentTypeEncodesLikeJson) {
  const std::string text = EncodeOrDie(MakeEventMessage("ping", "", Blob::FromString("{}")));
  EXPECT_EQ(text, R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}}})");
}

TEST(JsonRpcFrameTest, NotificationsDecodeBackToTheBinaryWireMessage) {
  const Message original =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})"));
  const JsonRpcStreamFrame frame = DecodeOrDie(EncodeOrDie(original));
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kEvent);
  EXPECT_EQ(frame.message, original);
}

TEST(JsonRpcFrameTest, DecodeIsInsensitiveToMemberOrderAndWhitespace) {
  // What a hand-written browser client actually sends: JSON.stringify's
  // insertion order, or pretty-printed text. Same Message either way.
  const JsonRpcStreamFrame compact = DecodeOrDie(
      R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"text":"hi"}}})");
  const JsonRpcStreamFrame reordered =
      DecodeOrDie(" { \"params\" : { \"payload\" : { \"text\" : \"hi\" } , \"id\" : 1 } ,"
                  " \"method\" : \"message\" , \"jsonrpc\" : \"2.0\" } ");
  EXPECT_EQ(compact.message, reordered.message);
}

TEST(JsonRpcFrameTest, NonIntegerIdsEchoAndMatch) {
  // The opening id is whatever the opening call carried — matched by
  // value, echoed verbatim. Type matters: 1 and "1" are different calls.
  const Document string_id("abc-1");
  const std::string text =
      EncodeOrDie(MakeEventMessage("ping", "", Blob::FromString("{}")), string_id);
  EXPECT_EQ(text, R"({"jsonrpc":"2.0","method":"ping","params":{"id":"abc-1","payload":{}}})");
  EXPECT_EQ(DecodeOrDie(text, string_id).kind, JsonRpcStreamFrame::Kind::kEvent);
  ExpectDecodeRefusal(text, "string echo against integer id");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":"1","payload":{}}})",
                      "\"1\" is not 1");
}

TEST(JsonRpcFrameTest, ExceptionsCannotRideNotifications) {
  // Terminal envelopes are the generated serve path's business — it owns
  // the @httpError table; the wrapper never guesses a code (ADR-0023).
  ExpectEncodeRefusal(
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})")),
      "exception message");
}

TEST(JsonRpcFrameTest, HeadersBeyondTheEnvelopeCannotRideTheWire) {
  // The notification has no header channel; encode refuses what the wire
  // cannot represent rather than dropping it (ADR-0014's rule).
  Message extra = MakeEventMessage("ping", "application/json", Blob::FromString("{}"));
  extra.headers.push_back({"x-app-extra", std::string("boom")});
  ExpectEncodeRefusal(extra, "extra header");

  ExpectEncodeRefusal(
      Message{.headers = {{":event-type", "chat"}}, .payload = Blob::FromString("{}")},
      "no :message-type");
}

TEST(JsonRpcFrameTest, NonJsonPayloadsAndContentTypesAreRefused) {
  ExpectEncodeRefusal(MakeEventMessage("ping", "application/cbor", Blob::FromString(R"({"n":1})")),
                      "cbor content type");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("[1,2]")), "array payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob()), "empty payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("{not json")),
                      "malformed payload");
}

TEST(JsonRpcFrameTest, TheTerminalResultClassifiesAsTheCleanEnd) {
  const JsonRpcStreamFrame frame = DecodeOrDie(R"({"jsonrpc":"2.0","result":{},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kResult);
  EXPECT_EQ(frame.result, Document(DocumentMap()));
  // The value is preserved but not policed — whatever rides here when
  // initial-response support lands is terminal either way.
  const JsonRpcStreamFrame carried =
      DecodeOrDie(R"({"jsonrpc":"2.0","result":{"note":"bye"},"id":1})");
  ASSERT_TRUE(carried.result.is_map());
  ASSERT_NE(carried.result.Find("note"), nullptr);
}

TEST(JsonRpcFrameTest, TheTerminalErrorClassifiesAsTheExceptionMessage) {
  // The unary error-object convention, unchanged (ADR-0023): data.__type
  // names the exception, data is the modeled payload.
  const JsonRpcStreamFrame frame = DecodeOrDie(
      R"({"jsonrpc":"2.0","error":{"code":409,"message":"kicked",)"
      R"("data":{"__type":"example.chat#Kicked","by":"mod"}},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kException);
  ASSERT_NE(frame.message.FindString(":exception-type"), nullptr);
  EXPECT_EQ(*frame.message.FindString(":exception-type"), "example.chat#Kicked");
  // The error message fills a data object that carries none — the unary
  // client's fallback, mirrored — and __type rides along harmlessly.
  EXPECT_EQ(frame.message.payload.ToString(),
            R"({"__type":"example.chat#Kicked","by":"mod","message":"kicked"})");
}

TEST(JsonRpcFrameTest, ADataMessageMemberIsNotOverwritten) {
  const JsonRpcStreamFrame frame = DecodeOrDie(
      R"({"jsonrpc":"2.0","error":{"code":400,"message":"outer",)"
      R"("data":{"__type":"X","message":"modeled"}},"id":1})");
  EXPECT_EQ(frame.message.payload.ToString(), R"({"__type":"X","message":"modeled"})");
}

TEST(JsonRpcFrameTest, AnUntypedErrorFallsBackToTheGenericException) {
  // A foreign server may omit data or __type; the fallback type matches no
  // modeled shape, so generated decoders surface the generic terminal
  // error instead of misclassifying.
  const JsonRpcStreamFrame frame =
      DecodeOrDie(R"({"jsonrpc":"2.0","error":{"code":-32601,"message":"unknown method"},"id":1})");
  EXPECT_EQ(frame.kind, JsonRpcStreamFrame::Kind::kException);
  ASSERT_NE(frame.message.FindString(":exception-type"), nullptr);
  EXPECT_EQ(*frame.message.FindString(":exception-type"), "JsonRpcError");
  EXPECT_EQ(frame.message.payload.ToString(), R"({"message":"unknown method"})");
}

TEST(JsonRpcFrameTest, ResponsesAndEchoesForAForeignIdAreRefused) {
  // One stream per socket: every frame belongs to the opening call.
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","result":{},"id":2})", "foreign response id");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","result":{}})", "missing response id");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":2,"payload":{}}})",
                      "foreign notification id");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"payload":{}}})",
                      "missing notification id echo");
}

TEST(JsonRpcFrameTest, ARequestEnvelopeAfterTheOpeningCallIsRefused) {
  // A method member plus a top-level id is a second call — there is no
  // second call on a one-stream socket (multiplexing is the door the id
  // echo leaves open, not a thing this wire speaks).
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{},"id":1})",
                      "request envelope");
}

TEST(JsonRpcFrameTest, TheFailClosedBankRefusesEveryMalformedEnvelope) {
  ExpectDecodeRefusal("not json at all", "not JSON");
  ExpectDecodeRefusal("", "empty text");
  ExpectDecodeRefusal("null", "null envelope");
  ExpectDecodeRefusal("[]", "array envelope");
  ExpectDecodeRefusal("{}", "empty envelope");
  ExpectDecodeRefusal(R"({"method":"ping","params":{"id":1,"payload":{}}})", "missing jsonrpc");
  ExpectDecodeRefusal(R"({"jsonrpc":"1.0","method":"ping","params":{"id":1,"payload":{}}})",
                      "wrong version");
  ExpectDecodeRefusal(R"({"jsonrpc":2,"method":"ping","params":{"id":1,"payload":{}}})",
                      "non-string version");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}},"x":1})",
                      "unknown member");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}},)"
                      R"("result":{}})",
                      "notification mixed with response");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","id":1})", "neither notification nor response");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"","params":{"id":1,"payload":{}}})",
                      "empty method");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":5,"params":{"id":1,"payload":{}}})",
                      "non-string method");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping"})", "missing params");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":[1]})", "array params");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{},"x":1}})",
                      "unknown params member");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1}})", "missing payload");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":[1]}})",
                      "array payload");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","result":{},"error":{"code":1},"id":1})",
                      "both result and error");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":[1],"id":1})", "array error");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":{},"id":1})", "missing code");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":{"code":"x"},"id":1})", "non-integer code");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":{"code":1,"message":5},"id":1})",
                      "non-string message");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":{"code":1,"data":[1]},"id":1})",
                      "non-object data");
  ExpectDecodeRefusal(R"({"jsonrpc":"2.0","error":{"code":1,"extra":true},"id":1})",
                      "unknown error member");
}

TEST(JsonRpcFrameTest, TheSizeCeilingHoldsInBothDirections) {
  const std::string huge_payload = R"({"blob":")" + std::string(kMaxMessageBytes, 'a') + R"("})";
  ExpectEncodeRefusal(MakeEventMessage("big", "", Blob::FromString(huge_payload)),
                      "oversized encode");
  const std::string huge_frame = R"({"jsonrpc":"2.0","method":"big","params":{"id":1,"payload":)" +
                                 huge_payload + "}}";
  ExpectDecodeRefusal(huge_frame, "oversized decode");
}

}  // namespace
}  // namespace smithy::eventstream
