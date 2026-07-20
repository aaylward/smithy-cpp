// Pins ADR-0016's typed session over the in-memory pair: EventStream
// plumbs its two codec functions onto the WebSocket contract — typed
// round-trips both ways, decode failures (received exceptions) terminal
// with a close, clean close as nullopt, and error passthrough that leaves
// encode failures non-fatal.

#include "smithy/eventstream/event_stream.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace smithy::eventstream {
namespace {

// Distinct client-to-server and server-to-client types, so a swapped
// type parameter cannot compile away silently.
struct Ping {
  int number = 0;
};
struct Pong {
  std::string text;
};

Outcome<Message> EncodePing(const Ping& ping) {
  return Message{.headers = {{":event-type", "ping"}},
                 .payload = Blob::FromString(std::to_string(ping.number))};
}

Outcome<Ping> DecodePing(const Message& message) {
  const std::string* type = message.FindString(":event-type");
  if (type == nullptr || *type != "ping") {
    return Error::Serialization("not a ping");
  }
  return Ping{std::stoi(message.payload.ToString())};
}

Outcome<Message> EncodePong(const Pong& pong) {
  return Message{.headers = {{":event-type", "pong"}}, .payload = Blob::FromString(pong.text)};
}

// The generated-decoder shape: a modeled exception message decodes to the
// typed Error, everything else to the event.
Outcome<Pong> DecodePong(const Message& message) {
  if (const std::string* exception = message.FindString(":exception-type"); exception != nullptr) {
    return Error::Modeled(*exception, message.payload.ToString());
  }
  return Pong{message.payload.ToString()};
}

using ClientStream = EventStream<Ping, Pong>;
using ServerStream = EventStream<Pong, Ping>;

TEST(EventStreamTest, TypedEventsRoundTripBothDirections) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  ServerStream server(server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client.Send(Ping{41}).ok());
  auto ping = server.Receive();
  ASSERT_TRUE(ping.ok() && ping->has_value());
  EXPECT_EQ((*ping)->number, 41);

  ASSERT_TRUE(server.Send(Pong{"forty-two"}).ok());
  auto pong = client.Receive();
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "forty-two");
}

TEST(EventStreamTest, TheBorrowedSocketConstructorServesTheServerPath) {
  // on_websocket lends a WebSocket&; the stream borrows it for the serve
  // callback's lifetime.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);
  ServerStream server(*server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client.Send(Ping{7}).ok());
  auto ping = server.Receive();
  ASSERT_TRUE(ping.ok() && ping->has_value());
  EXPECT_EQ((*ping)->number, 7);
  ASSERT_TRUE(server.Send(Pong{"seven"}).ok());
  auto pong = client.Receive();
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "seven");
}

TEST(EventStreamTest, ADecodedExceptionEndsTheStreamWithItsError) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  // The peer sends a modeled exception message, then keeps listening.
  ASSERT_TRUE(server_socket
                  ->Send(Message{.headers = {{":exception-type", "RoomFull"}},
                                 .payload = Blob::FromString("room is full")})
                  .ok());

  // The decoder's error surfaces as the Receive outcome...
  const auto received = client.Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kModeled);
  EXPECT_EQ(received.error().code(), "RoomFull");
  EXPECT_EQ(received.error().message(), "room is full");

  // ...and the exception was terminal (ADR-0016): the stream closed the
  // session, so this end cannot send and the peer sees the clean close.
  const auto after = client.Send(Ping{1});
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error().kind(), ErrorKind::kTransport);
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok());
  EXPECT_FALSE(at_peer->has_value());
}

TEST(EventStreamTest, AnUndecodableMessageIsEquallyTerminal) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ServerStream server(server_socket, EncodePong, DecodePing);

  ASSERT_TRUE(client_socket->Send(Message{.headers = {{":event-type", "not-a-ping"}}}).ok());
  const auto received = server.Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
  EXPECT_FALSE(server.Send(Pong{"x"}).ok());
}

TEST(EventStreamTest, ThePeersCleanCloseIsNullopt) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  ASSERT_TRUE(server_socket->Send(Message{.payload = Blob::FromString("last words")}).ok());
  server_socket->Close();

  // The message queued before the close still arrives...
  auto last = client.Receive();
  ASSERT_TRUE(last.ok() && last->has_value());
  EXPECT_EQ((*last)->text, "last words");
  // ...then the stream's natural end, sticky across calls.
  auto closed = client.Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
  auto again = client.Receive();
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->has_value());
}

TEST(EventStreamTest, SendAfterCloseIsATransportError) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream client(client_socket, EncodePing, DecodePong);

  client.Close();
  client.Close();  // idempotent, per the WebSocket contract
  const auto outcome = client.Send(Ping{1});
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
}

TEST(EventStreamTest, AnEncoderFailureSurfacesWithoutEndingTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ClientStream failing(
      client_socket,
      [](const Ping& ping) -> Outcome<Message> {
        if (ping.number < 0) return Error::Validation("negative ping");
        return EncodePing(ping);
      },
      DecodePong);

  const auto refused = failing.Send(Ping{-1});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // The session was never touched: the next Send still goes through.
  ASSERT_TRUE(failing.Send(Ping{5}).ok());
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
  EXPECT_EQ((*at_peer)->payload.ToString(), "5");
}

}  // namespace
}  // namespace smithy::eventstream
