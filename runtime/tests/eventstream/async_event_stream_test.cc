// Pins ADR-0019 over the in-memory pair: the completion-driven socket
// primitives (park/complete, one-outstanding refusals, close semantics),
// the coroutine adapter (co_await Receive/Send with EventStream's exact
// terminal behaviors), and the handle's async send with its
// revocation-spanning pin. The Beast halves of the same contracts live in
// beast_websocket_test.cc; the registry's async delivery in
// session_registry_test.cc.

#include "smithy/eventstream/async_event_stream.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"

namespace smithy::eventstream {
namespace {

constexpr std::size_t kWireDepth = http::InMemoryWebSocketPair::kQueueDepth;

// The event_stream_test codec shapes, minimal: distinct directions so a
// swapped parameter cannot compile away.
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

Outcome<Pong> DecodePong(const Message& message) { return Pong{message.payload.ToString()}; }

Message RawPing(int number) {
  return Message{.headers = {{":event-type", "ping"}},
                 .payload = Blob::FromString(std::to_string(number))};
}

// A one-shot completion mailbox: tests park an async op and assert on what
// (and that exactly one thing) arrived.
template <typename T>
class Mailbox {
 public:
  void Post(T value) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ASSERT_FALSE(value_.has_value()) << "completion fired twice";
      value_.emplace(std::move(value));
    }
    ready_.notify_all();
  }

  T Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return value_.has_value(); });
    T value = std::move(*value_);
    value_.reset();
    return value;
  }

  bool Empty() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return !value_.has_value();
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<T> value_;
};

// ---------------------------------------------------------------------------
// The raw pair primitives.
// ---------------------------------------------------------------------------

TEST(PairAsyncTest, AParkedReceiveCompletesOnThePeersSend) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  EXPECT_TRUE(received.Empty());  // parked: nothing sent yet

  ASSERT_TRUE(b->Send(RawPing(7)).ok());
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "7");
}

TEST(PairAsyncTest, AReadyMessageCompletesImmediately) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  ASSERT_TRUE(b->Send(RawPing(1)).ok());
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "1");
}

TEST(PairAsyncTest, ASecondOutstandingReceiveIsRefused) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> first;
  Mailbox<Outcome<std::optional<Message>>> second;
  a->ReceiveAsync([&](Outcome<std::optional<Message>> message) { first.Post(std::move(message)); });
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { second.Post(std::move(message)); });

  auto refused = second.Wait();  // inline refusal, the one-outstanding rule
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);

  ASSERT_TRUE(b->Send(RawPing(2)).ok());  // the first still completes
  auto outcome = first.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
}

TEST(PairAsyncTest, AsyncSendParksOnTheFullWireAndDrainsInOrder) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    ASSERT_TRUE(a->Send(RawPing(static_cast<int>(i))).ok());
  }
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(RawPing(99), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  EXPECT_TRUE(sent.Empty());  // parked behind the bound

  // A second send-class op while one is parked is refused.
  Mailbox<Outcome<Unit>> refused;
  a->SendAsync(RawPing(100), [&](Outcome<Unit> outcome) { refused.Post(std::move(outcome)); });
  auto refusal = refused.Wait();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  // Draining one message absorbs the parked send; FIFO holds end to end.
  for (std::size_t i = 0; i <= kWireDepth; ++i) {
    auto message = b->Receive();
    ASSERT_TRUE(message.ok() && message->has_value());
    const int expected = i < kWireDepth ? static_cast<int>(i) : 99;
    EXPECT_EQ((*message)->payload.ToString(), std::to_string(expected));
  }
  EXPECT_TRUE(sent.Wait().ok());
}

TEST(PairAsyncTest, CloseCompletesParkedOperations) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    ASSERT_TRUE(a->Send(RawPing(static_cast<int>(i))).ok());
  }
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(RawPing(99), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });

  b->Close();
  // The parked receive gets the clean end (its queue was empty — the
  // parked-receive invariant); the parked send gets the transport error.
  auto end = received.Wait();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  auto dead = sent.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

TEST(PairAsyncTest, ThePairReportsAsyncSupportAndDefaultsRefuse) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  EXPECT_TRUE(a->SupportsAsync());

  // A WebSocket that overrides nothing keeps compiling and refuses politely
  // — the fallback every layer above keys on.
  class BlockingOnly final : public http::WebSocket {
   public:
    Outcome<std::optional<Message>> Receive() override { return std::optional<Message>(); }
    Outcome<Unit> Send(const Message&) override { return Unit{}; }
    void Close() override {}
  };
  BlockingOnly plain;
  EXPECT_FALSE(plain.SupportsAsync());
  Mailbox<Outcome<Unit>> sent;
  plain.SendAsync(RawPing(1), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  auto refused = sent.Wait();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
}

// ---------------------------------------------------------------------------
// The coroutine adapter.
// ---------------------------------------------------------------------------

using AsyncServer = AsyncEventStream<Pong, Ping>;

// The canonical Detached echo loop: pongs every ping's number back as text,
// ends on the client's close (or any terminal outcome), flags its exit.
Detached EchoLoop(std::shared_ptr<http::WebSocket> socket, std::atomic<bool>* done) {
  AsyncServer stream(std::move(socket), EncodePong, DecodePing);
  while (true) {
    auto ping = co_await stream.Receive();
    if (!ping.ok() || !ping->has_value()) break;
    auto sent = co_await stream.Send(Pong{"pong-" + std::to_string((*ping)->number)});
    if (!sent.ok()) break;
  }
  *done = true;
}

TEST(AsyncEventStreamTest, ADetachedLoopEchoesAndEndsOnTheCleanClose) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<bool> done{false};
  EchoLoop(server_socket, &done);

  EventStream<Ping, Pong> client(client_socket, EncodePing, DecodePong);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(client.Send(Ping{i}).ok());
    auto pong = client.Receive();
    ASSERT_TRUE(pong.ok() && pong->has_value());
    EXPECT_EQ((*pong)->text, "pong-" + std::to_string(i));
  }
  EXPECT_FALSE(done.load());  // the loop is parked in co_await, not gone

  client.Close();
  // The parked receive completes with the clean end on this thread (the
  // pair's completion context), so the loop has unwound by the time Close
  // returns — but don't rely on that: poll briefly for the flag.
  for (int i = 0; i < 100 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(done.load());
}

TEST(AsyncEventStreamTest, AwaitedSendBackpressuresWithoutAThread) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<int> sent_count{0};
  std::atomic<bool> done{false};

  // A pusher loop: fires kWireDepth + 2 sends; the wire bound parks it
  // after kWireDepth — with no thread blocked anywhere.
  [](std::shared_ptr<http::WebSocket> socket, std::atomic<int>* sent_count,
     std::atomic<bool>* done) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    for (std::size_t i = 0; i < kWireDepth + 2; ++i) {
      auto sent = co_await stream.Send(Pong{"p-" + std::to_string(i)});
      if (!sent.ok()) break;
      sent_count->fetch_add(1);
    }
    *done = true;
  }(server_socket, &sent_count, &done);

  EXPECT_EQ(sent_count.load(), static_cast<int>(kWireDepth));  // parked at the bound
  EXPECT_FALSE(done.load());

  // Draining resumes the coroutine on this thread; FIFO order holds.
  EventStream<Ping, Pong> client(client_socket, EncodePing, DecodePong);
  for (std::size_t i = 0; i < kWireDepth + 2; ++i) {
    auto pong = client.Receive();
    ASSERT_TRUE(pong.ok() && pong->has_value());
    EXPECT_EQ((*pong)->text, "p-" + std::to_string(i));
  }
  EXPECT_TRUE(done.load());
}

TEST(AsyncEventStreamTest, AnUndecodableMessageIsTerminalThroughTheAdapter) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Ping>>> received;

  [](std::shared_ptr<http::WebSocket> socket,
     Mailbox<Outcome<std::optional<Ping>>>* received) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    auto ping = co_await stream.Receive();
    received->Post(std::move(ping));
  }(server_socket, &received);

  ASSERT_TRUE(client_socket->Send(Message{.headers = {{":event-type", "not-a-ping"}}}).ok());
  auto outcome = received.Wait();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kSerialization);
  // Terminal, like EventStream (ADR-0016): the adapter closed the session.
  auto closed = client_socket->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

TEST(AsyncEventStreamTest, AnEncoderFailureSurfacesWithoutSuspendingOrEndingTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<Unit>> first;
  Mailbox<Outcome<Unit>> second;

  [](std::shared_ptr<http::WebSocket> socket, Mailbox<Outcome<Unit>>* first,
     Mailbox<Outcome<Unit>>* second) -> Detached {
    AsyncEventStream<Ping, Pong> stream(
        std::move(socket),
        [](const Ping& ping) -> Outcome<Message> {
          if (ping.number < 0) return Error::Validation("negative ping");
          return EncodePing(ping);
        },
        DecodePong);
    first->Post(co_await stream.Send(Ping{-1}));
    second->Post(co_await stream.Send(Ping{5}));
  }(client_socket, &first, &second);

  auto refused = first.Wait();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  EXPECT_TRUE(second.Wait().ok());  // the session was never touched
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
  EXPECT_EQ((*at_peer)->payload.ToString(), "5");
}

TEST(AsyncEventStreamTest, SharedHandlesOutliveTheLoopAndItsStream) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<EventStreamHandle<Pong>> minted;
  std::atomic<bool> done{false};

  [](std::shared_ptr<http::WebSocket> socket, Mailbox<EventStreamHandle<Pong>>* minted,
     std::atomic<bool>* done) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    minted->Post(stream.Share());
    (void)co_await stream.Receive();  // parked until the client closes
    *done = true;
  }(server_socket, &minted, &done);

  EventStreamHandle<Pong> handle = minted.Wait();
  ASSERT_TRUE(handle.Send(Pong{"from-outside-the-loop"}).ok());
  auto at_client = client_socket->Receive();
  ASSERT_TRUE(at_client.ok() && at_client->has_value());
  EXPECT_EQ((*at_client)->payload.ToString(), "from-outside-the-loop");

  // The loop ends, its frame — and the stream inside it — die, and the
  // handle degrades to the stale-handle contract instead of dangling.
  client_socket->Close();
  for (int i = 0; i < 100 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(done.load());
  const auto stale = handle.Send(Pong{"too-late"});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().kind(), ErrorKind::kTransport);
}

TEST(AsyncEventStreamTest, HandleSendAsyncCompletesAndFailsSoftlyAfterRevocation) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<Unit>> live;
  Mailbox<Outcome<Unit>> stale;
  std::optional<EventStreamHandle<Pong>> handle;
  {
    AsyncServer stream(server_socket, EncodePong, DecodePing);
    handle = stream.Share();
    handle->SendAsync(Pong{"async"}, [&](Outcome<Unit> sent) { live.Post(std::move(sent)); });
    EXPECT_TRUE(live.Wait().ok());
    auto at_client = client_socket->Receive();
    ASSERT_TRUE(at_client.ok() && at_client->has_value());
  }  // ~AsyncEventStream revokes: close, drain pins, null the socket

  handle->SendAsync(Pong{"late"}, [&](Outcome<Unit> sent) { stale.Post(std::move(sent)); });
  auto dead = stale.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

}  // namespace
}  // namespace smithy::eventstream
