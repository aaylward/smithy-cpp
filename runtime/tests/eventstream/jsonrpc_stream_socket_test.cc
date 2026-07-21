// JsonRpcStreamSocket over the in-memory pair (ADR-0023): the translation
// wrapper runs ABOVE the transport, so unlike ADR-0018's negotiated mode
// the pair exercises the actual JSON-RPC text — one end unwrapped IS the
// wire view. Covers the event round trip through both facades (blocking
// and async), the terminal-response classification (result = clean end,
// error = exception message), the exception-send refusal, and the
// fail-closed posture on malformed and mis-framed inbound traffic.

#include "smithy/eventstream/jsonrpc_stream_socket.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "smithy/core/document.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket_pair.h"

namespace smithy::eventstream {
namespace {

// A headerless raw-text message: what the wire (and the unwrapped pair
// end) carries — the payload is one JSON-RPC envelope.
Message Raw(std::string text) {
  Message message;
  message.payload = Blob::FromString(std::move(text));
  return message;
}

std::shared_ptr<JsonRpcStreamSocket> Wrap(std::shared_ptr<http::WebSocket> inner) {
  return std::make_shared<JsonRpcStreamSocket>(std::move(inner), Document(1));
}

TEST(JsonRpcStreamSocketTest, EventsRoundTripBetweenTwoWrappedEnds) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);
  auto server = Wrap(right);

  const Message event =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})"));
  ASSERT_TRUE(client->Send(event).ok());
  auto received = server->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, event);

  // Full duplex: the same translation runs the other way.
  ASSERT_TRUE(server->Send(event).ok());
  auto echoed = client->Receive();
  ASSERT_TRUE(echoed.ok());
  ASSERT_TRUE(echoed->has_value());
  EXPECT_EQ(**echoed, event);
}

TEST(JsonRpcStreamSocketTest, TheWireCarriesThePinnedNotificationText) {
  // One end unwrapped: what it receives is the wire — a headerless
  // message whose payload is the notification text, byte-pinned.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  ASSERT_TRUE(
      client->Send(MakeEventMessage("message", "application/json", Blob::FromString(R"({"n":1})")))
          .ok());
  auto raw = right->Receive();
  ASSERT_TRUE(raw.ok());
  ASSERT_TRUE(raw->has_value());
  EXPECT_TRUE((**raw).headers.empty());
  EXPECT_EQ((**raw).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"n":1}}})");
}

TEST(JsonRpcStreamSocketTest, TheTerminalResultIsTheCleanEnd) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto received = client->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  // The terminal result never surfaces as a message — the stream ends the
  // way a peer close ends it, and the server's close follows right behind.
  EXPECT_FALSE(received->has_value());
}

TEST(JsonRpcStreamSocketTest, TheTerminalErrorArrivesAsTheExceptionMessage) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  ASSERT_TRUE(right
                  ->Send(Raw(R"({"jsonrpc":"2.0","error":{"code":409,"message":"kicked",)"
                             R"("data":{"__type":"Kicked","by":"mod"}},"id":1})"))
                  .ok());
  auto received = client->Receive();
  ASSERT_TRUE(received.ok());
  ASSERT_TRUE(received->has_value());
  // Exactly the exception Message the binary wire would have carried —
  // ADR-0016's terminal contract downstream (EventStream, the generated
  // decoders) applies unchanged.
  ASSERT_NE((**received).FindString(":exception-type"), nullptr);
  EXPECT_EQ(*(**received).FindString(":exception-type"), "Kicked");
  EXPECT_EQ((**received).payload.ToString(),
            R"({"__type":"Kicked","by":"mod","message":"kicked"})");
}

TEST(JsonRpcStreamSocketTest, ExceptionSendsAreRefusedAndTheSessionSurvives) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto server = Wrap(right);

  auto refused = server->Send(
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})")));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // The refusal is the Send contract's Validation: session untouched —
  // the terminal error envelope is sent raw by the generated serve path.
  EXPECT_TRUE(server->Send(MakeEventMessage("ping", "", Blob::FromString("{}"))).ok());
}

TEST(JsonRpcStreamSocketTest, MalformedInboundTextIsSerializationTerminal) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  ASSERT_TRUE(right->Send(Raw("not json")).ok());
  auto received = client->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
}

TEST(JsonRpcStreamSocketTest, EnvelopeFramedInboundMessagesAreRefused) {
  // A peer that speaks the eventstream envelope wire into a JSON-RPC
  // stream (a mis-wired test, a wrong-mode transport) is a protocol
  // violation, not a message.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  ASSERT_TRUE(
      right->Send(MakeEventMessage("ping", "application/json", Blob::FromString("{}"))).ok());
  auto received = client->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
}

TEST(JsonRpcStreamSocketTest, TheAsyncTwinsTranslateTheSameWay) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);
  auto server = Wrap(right);
  ASSERT_TRUE(client->SupportsAsync());

  // The pair completes inline; the assertions run before the calls return.
  bool sent = false;
  client->SendAsync(MakeEventMessage("message", "application/json", Blob::FromString(R"({"n":1})")),
                    [&sent](Outcome<Unit> outcome) {
                      EXPECT_TRUE(outcome.ok()) << outcome.error().message();
                      sent = true;
                    });
  EXPECT_TRUE(sent);

  bool received = false;
  server->ReceiveAsync([&received](Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok()) << message.error().message();
    ASSERT_TRUE(message->has_value());
    ASSERT_NE((**message).FindString(":event-type"), nullptr);
    EXPECT_EQ(*(**message).FindString(":event-type"), "message");
    received = true;
  });
  EXPECT_TRUE(received);

  // Terminal classification runs on the async path too.
  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  bool ended = false;
  client->ReceiveAsync([&ended](Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok());
    EXPECT_FALSE(message->has_value());
    ended = true;
  });
  EXPECT_TRUE(ended);
}

TEST(JsonRpcStreamSocketTest, AnAsyncEncodeRefusalCompletesInline) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);

  bool refused = false;
  client->SendAsync(MakeExceptionMessage("Kicked", "", Blob::FromString("{}")),
                    [&refused](Outcome<Unit> outcome) {
                      ASSERT_FALSE(outcome.ok());
                      EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
                      refused = true;
                    });
  EXPECT_TRUE(refused);
}

TEST(JsonRpcStreamSocketTest, TheBorrowingFormTranslatesLikeTheOwningOne) {
  // The blocking serve seam's shape: the route borrows its socket, so the
  // wrapper borrows too (EventStream's borrowed-constructor mirror).
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  JsonRpcStreamSocket client(*left, Document(1));

  const Message event = MakeEventMessage("ping", "application/json", Blob::FromString("{}"));
  ASSERT_TRUE(client.Send(event).ok());
  auto raw = right->Receive();
  ASSERT_TRUE(raw.ok() && raw->has_value());
  EXPECT_EQ((**raw).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}}})");

  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto received = client.Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());
}

TEST(JsonRpcStreamSocketTest, CloseAndThePeersCleanCloseDelegate) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left);
  auto server = Wrap(right);

  client->Close();
  auto received = server->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close, untranslated
}

}  // namespace
}  // namespace smithy::eventstream
