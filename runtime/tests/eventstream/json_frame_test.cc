// Pins ADR-0018's JSON-text wire encoding: the exact envelope text the
// encoder mints (byte-pinned — text frames ARE the wire), the round trip
// back to the Message the binary wire would carry, the scoping refusals
// (non-JSON content types, extra headers, non-object payloads), and the
// fail-closed bank — every malformed envelope must surface as
// Error::Serialization, never as a half-understood message.

#include "smithy/eventstream/json_frame.h"

#include <gtest/gtest.h>

#include <string>

#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/frame.h"

namespace smithy::eventstream {
namespace {

std::string EncodeOrDie(const Message& message) {
  auto text = EncodeJsonFrame(message);
  EXPECT_TRUE(text.ok()) << text.error().message();
  return text.ok() ? *text : std::string();
}

Message DecodeOrDie(std::string_view text) {
  auto message = DecodeJsonFrame(text);
  EXPECT_TRUE(message.ok()) << message.error().message();
  return message.ok() ? *message : Message{};
}

void ExpectEncodeRefusal(const Message& message, const char* why) {
  const auto text = EncodeJsonFrame(message);
  ASSERT_FALSE(text.ok()) << why;
  EXPECT_EQ(text.error().kind(), ErrorKind::kValidation) << why;
}

void ExpectDecodeRefusal(std::string_view text, const char* why) {
  const auto message = DecodeJsonFrame(text);
  ASSERT_FALSE(message.ok()) << why;
  EXPECT_EQ(message.error().kind(), ErrorKind::kSerialization) << why;
}

TEST(JsonFrameTest, EventMessagesRenderThePinnedEnvelopeText) {
  // Byte-pinned: this text is what a browser's onmessage receives. The
  // codec's JSON output is deterministic (sorted keys, compact), so
  // string equality is wire equality.
  const std::string text = EncodeOrDie(
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})")));
  EXPECT_EQ(text, R"({"event":"message","payload":{"text":"hi"}})");
}

TEST(JsonFrameTest, ExceptionMessagesRenderTheExceptionArm) {
  const std::string text = EncodeOrDie(
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})")));
  EXPECT_EQ(text, R"({"exception":"Kicked","payload":{"by":"mod"}})");
}

TEST(JsonFrameTest, AnAbsentContentTypeEncodesLikeJson) {
  // ParseEnvelope tolerates an absent :content-type (the protocol layer
  // knows its own); the JSON wire extends the same tolerance.
  const std::string text = EncodeOrDie(MakeEventMessage("ping", "", Blob::FromString("{}")));
  EXPECT_EQ(text, R"({"event":"ping","payload":{}})");
}

TEST(JsonFrameTest, ContentTypeParametersAndCaseAreTolerated) {
  const std::string text = EncodeOrDie(
      MakeEventMessage("ping", "Application/JSON; charset=utf-8", Blob::FromString(R"({"n":1})")));
  EXPECT_EQ(text, R"({"event":"ping","payload":{"n":1}})");
}

TEST(JsonFrameTest, EncodedFramesDecodeBackToTheBinaryWireMessage) {
  // The transport translates both directions; above the socket the two
  // wires carry the same Message. content-type comes back stamped even
  // when the original omitted it — the JSON wire's payloads are always
  // application/json.
  const Message original =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})"));
  EXPECT_EQ(DecodeOrDie(EncodeOrDie(original)), original);

  const Message exception =
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})"));
  EXPECT_EQ(DecodeOrDie(EncodeOrDie(exception)), exception);
}

TEST(JsonFrameTest, NonJsonContentTypesCannotRideTheTextWire) {
  // The simpleRestJson scoping (ADR-0018), enforced where the wire is
  // minted: a CBOR event stream on a JSON-mode session fails on the first
  // send instead of shipping CBOR bytes in a text frame.
  ExpectEncodeRefusal(MakeEventMessage("ping", "application/cbor", Blob::FromString(R"({"n":1})")),
                      "cbor content type");
}

TEST(JsonFrameTest, HeadersBeyondTheEnvelopeCannotRideTheTextWire) {
  // The JSON envelope has no header channel; encode refuses what the wire
  // cannot represent rather than dropping it (ADR-0014's rule).
  Message message = MakeEventMessage("ping", "application/json", Blob::FromString("{}"));
  message.headers.push_back({"x-app-extra", std::string("boom")});
  ExpectEncodeRefusal(message, "extra header");
}

TEST(JsonFrameTest, NonObjectAndNonJsonPayloadsAreRefused) {
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("[1,2]")), "array payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("\"text\"")), "string payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob()), "empty payload");
  ExpectEncodeRefusal(MakeEventMessage("ping", "", Blob::FromString("{not json")),
                      "malformed payload");
}

TEST(JsonFrameTest, MessagesWithoutAnEnvelopeAreRefused) {
  // Only envelope-bearing messages can ride this wire; a raw Message with
  // no :message-type has no JSON rendering.
  ExpectEncodeRefusal(
      Message{.headers = {{":event-type", "chat"}}, .payload = Blob::FromString("{}")},
      "no :message-type");
}

TEST(JsonFrameTest, DecodeStampsTheFullEnvelope) {
  const Message message = DecodeOrDie(R"({"event":"message","payload":{"text":"hi"}})");
  const Message expected{.headers = {{":message-type", "event"},
                                     {":event-type", "message"},
                                     {":content-type", "application/json"}},
                         .payload = Blob::FromString(R"({"text":"hi"})")};
  EXPECT_EQ(message, expected);
}

TEST(JsonFrameTest, DecodeIsInsensitiveToMemberOrderAndWhitespace) {
  // What a hand-written browser client actually sends: JSON.stringify's
  // insertion order, or pretty-printed text. Same Message either way.
  const Message compact = DecodeOrDie(R"({"event":"message","payload":{"text":"hi"}})");
  const Message reordered =
      DecodeOrDie(" { \"payload\" : { \"text\" : \"hi\" } , \"event\" : \"message\" } ");
  EXPECT_EQ(compact, reordered);
}

TEST(JsonFrameTest, DecodeNormalizesThePayloadDialect) {
  // The payload is re-encoded through the runtime's JSON codec, so
  // generated serde sees one dialect (sorted keys, compact) regardless of
  // what the peer typed.
  const Message message = DecodeOrDie(R"({"event":"x","payload":{"b":1, "a":2}})");
  EXPECT_EQ(message.payload.ToString(), R"({"a":2,"b":1})");
}

TEST(JsonFrameTest, UnknownEventTypesAreTheGeneratedDecodersCall) {
  // The closed-union rule keeps its existing owner: this layer decodes any
  // member name; the generated decoder rejects unknown ones terminally,
  // exactly as in binary mode.
  const Message message = DecodeOrDie(R"({"event":"never-modeled","payload":{}})");
  ASSERT_NE(message.FindString(":event-type"), nullptr);
  EXPECT_EQ(*message.FindString(":event-type"), "never-modeled");
}

TEST(JsonFrameTest, TheInitialResponseReservationTransposes) {
  // ADR-0016 reserves ":initial-response" out of member space via the
  // leading colon; the JSON wire carries it symmetrically.
  const Message message =
      MakeEventMessage(kInitialResponseEventType, "application/json", Blob::FromString("{}"));
  EXPECT_EQ(DecodeOrDie(EncodeOrDie(message)), message);
}

TEST(JsonFrameTest, TheFailClosedBankRefusesEveryMalformedEnvelope) {
  ExpectDecodeRefusal("not json at all", "not JSON");
  ExpectDecodeRefusal("[]", "array envelope");
  ExpectDecodeRefusal("42", "number envelope");
  ExpectDecodeRefusal("\"event\"", "string envelope");
  ExpectDecodeRefusal("{}", "empty envelope");
  ExpectDecodeRefusal(R"({"event":"x","payload":{},"id":7})", "unknown member");
  ExpectDecodeRefusal(R"({"event":"x","exception":"y","payload":{}})", "both discriminators");
  ExpectDecodeRefusal(R"({"payload":{}})", "neither discriminator");
  ExpectDecodeRefusal(R"({"event":5,"payload":{}})", "non-string event");
  ExpectDecodeRefusal(R"({"event":"","payload":{}})", "empty event");
  ExpectDecodeRefusal(R"({"exception":null,"payload":{}})", "null exception");
  ExpectDecodeRefusal(R"({"event":"x"})", "missing payload");
  ExpectDecodeRefusal(R"({"event":"x","payload":[1]})", "array payload");
  ExpectDecodeRefusal(R"({"event":"x","payload":"text"})", "string payload");
  ExpectDecodeRefusal(R"({"event":"x","payload":null})", "null payload");
}

TEST(JsonFrameTest, TheSizeCeilingHoldsInBothDirections) {
  // kMaxMessageBytes applies to the text frame — the symmetric-bounds line
  // (ADR-0014), extended to this wire.
  const std::string huge_payload = R"({"blob":")" + std::string(kMaxMessageBytes, 'a') + R"("})";
  ExpectEncodeRefusal(MakeEventMessage("big", "", Blob::FromString(huge_payload)),
                      "oversized encode");
  const std::string huge_frame =
      R"({"event":"big","payload":{"blob":")" + std::string(kMaxMessageBytes, 'a') + R"("}})";
  ExpectDecodeRefusal(huge_frame, "oversized decode");
}

}  // namespace
}  // namespace smithy::eventstream
