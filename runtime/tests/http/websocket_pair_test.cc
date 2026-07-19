// Pins the in-memory WebSocket pair (ADR-0016) to the WebSocket contract
// the Beast sessions implement: blocking round-trips both ways, full-duplex
// concurrency, bounded-queue backpressure, and close semantics — clean
// nullopt after draining, Error::Transport on Send, idempotence, and
// unblocking blocked calls from another thread.

#include "smithy/http/websocket_pair.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::http {
namespace {

using eventstream::Message;

Message Text(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = Blob::FromString(body)};
}

// Blocks until check() holds or the deadline passes; the polling half of
// timing-sensitive assertions (the sleeping half asserts stall).
bool WaitFor(const std::function<bool()>& check) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!check()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(WebSocketPairTest, MessagesRoundTripBothWays) {
  auto [a, b] = InMemoryWebSocketPair::Create();

  ASSERT_TRUE(a->Send(Text("chat", "from a")).ok());
  auto at_b = b->Receive();
  ASSERT_TRUE(at_b.ok() && at_b->has_value());
  EXPECT_EQ(**at_b, Text("chat", "from a"));

  ASSERT_TRUE(b->Send(Text("chat", "from b")).ok());
  auto at_a = a->Receive();
  ASSERT_TRUE(at_a.ok() && at_a->has_value());
  EXPECT_EQ(**at_a, Text("chat", "from b"));
}

TEST(WebSocketPairTest, FullDuplexPingPongAcrossTwoThreads) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr int kRounds = 100;

  std::thread peer([&b] {
    for (int i = 0; i < kRounds; ++i) {
      auto message = b->Receive();
      ASSERT_TRUE(message.ok() && message->has_value()) << "round " << i;
      Message reply = **message;
      reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
      ASSERT_TRUE(b->Send(reply).ok()) << "round " << i;
    }
  });

  for (int i = 0; i < kRounds; ++i) {
    ASSERT_TRUE(a->Send(Text("ping", std::to_string(i))).ok()) << "round " << i;
    auto reply = a->Receive();
    ASSERT_TRUE(reply.ok() && reply->has_value()) << "round " << i;
    EXPECT_EQ((*reply)->payload.ToString(), "echo:" + std::to_string(i)) << "round " << i;
  }
  peer.join();
}

TEST(WebSocketPairTest, BothDirectionsStreamConcurrently) {
  // Two senders pumping opposite directions at once, far past the queue
  // bound — progress requires the two directions not to block each other.
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr int kCount = 64;

  std::thread from_a([&a] {
    for (int i = 0; i < kCount; ++i) {
      ASSERT_TRUE(a->Send(Text("a", std::to_string(i))).ok()) << "message " << i;
    }
  });
  std::thread from_b([&b] {
    for (int i = 0; i < kCount; ++i) {
      ASSERT_TRUE(b->Send(Text("b", std::to_string(i))).ok()) << "message " << i;
    }
  });

  // Each direction arrives complete and in order.
  for (int i = 0; i < kCount; ++i) {
    auto at_b = b->Receive();
    ASSERT_TRUE(at_b.ok() && at_b->has_value()) << "message " << i;
    EXPECT_EQ((*at_b)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  for (int i = 0; i < kCount; ++i) {
    auto at_a = a->Receive();
    ASSERT_TRUE(at_a.ok() && at_a->has_value()) << "message " << i;
    EXPECT_EQ((*at_a)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  from_a.join();
  from_b.join();
}

TEST(WebSocketPairTest, AFullQueueBlocksTheSenderUntilTheReceiverDrains) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr std::size_t kDepth = 8;  // the documented Beast-parity bound
  constexpr std::size_t kTotal = kDepth + 4;

  std::atomic<std::size_t> sent{0};
  std::thread sender([&a, &sent] {
    for (std::size_t i = 0; i < kTotal; ++i) {
      ASSERT_TRUE(a->Send(Text("n", std::to_string(i))).ok()) << "message " << i;
      sent.fetch_add(1);
    }
  });

  // The sender reaches the bound and stalls there: no amount of waiting
  // lets message kDepth+1 through while nothing drains.
  ASSERT_TRUE(WaitFor([&sent] { return sent.load() == kDepth; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sent.load(), kDepth);

  // Each Receive frees exactly one slot.
  auto first = b->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->payload.ToString(), "0");
  ASSERT_TRUE(WaitFor([&sent] { return sent.load() == kDepth + 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sent.load(), kDepth + 1);

  // Draining the rest releases the sender completely, in order.
  for (std::size_t i = 1; i < kTotal; ++i) {
    auto message = b->Receive();
    ASSERT_TRUE(message.ok() && message->has_value()) << "message " << i;
    EXPECT_EQ((*message)->payload.ToString(), std::to_string(i)) << "message " << i;
  }
  sender.join();
  EXPECT_EQ(sent.load(), kTotal);
}

TEST(WebSocketPairTest, QueuedMessagesDrainBeforeTheCloseIsReported) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  ASSERT_TRUE(a->Send(Text("n", "1")).ok());
  ASSERT_TRUE(a->Send(Text("n", "2")).ok());
  a->Close();

  // The peer still owns what arrived before the close, in order...
  auto first = b->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->payload.ToString(), "1");
  auto second = b->Receive();
  ASSERT_TRUE(second.ok() && second->has_value());
  EXPECT_EQ((*second)->payload.ToString(), "2");
  // ...then the clean close, sticky across repeated calls.
  auto closed = b->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
  auto again = b->Receive();
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again->has_value());
}

TEST(WebSocketPairTest, SendFailsWithTransportOnceEitherEndCloses) {
  {
    auto [a, b] = InMemoryWebSocketPair::Create();
    a->Close();
    const auto own = a->Send(Text("n", "x"));
    ASSERT_FALSE(own.ok());
    EXPECT_EQ(own.error().kind(), ErrorKind::kTransport);
    const auto peer = b->Send(Text("n", "x"));
    ASSERT_FALSE(peer.ok());
    EXPECT_EQ(peer.error().kind(), ErrorKind::kTransport);
  }
  {
    // Symmetric from the other end.
    auto [a, b] = InMemoryWebSocketPair::Create();
    b->Close();
    EXPECT_FALSE(a->Send(Text("n", "x")).ok());
    EXPECT_FALSE(b->Send(Text("n", "x")).ok());
  }
}

TEST(WebSocketPairTest, CloseIsIdempotentFromBothEnds) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  a->Close();
  a->Close();
  b->Close();
  b->Close();
  auto at_a = a->Receive();
  ASSERT_TRUE(at_a.ok());
  EXPECT_FALSE(at_a->has_value());
  auto at_b = b->Receive();
  ASSERT_TRUE(at_b.ok());
  EXPECT_FALSE(at_b->has_value());
}

TEST(WebSocketPairTest, CloseUnblocksABlockedReceive) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  std::atomic<bool> unblocked{false};
  std::thread receiver([&b, &unblocked] {
    auto message = b->Receive();  // blocks: nothing was sent
    EXPECT_TRUE(message.ok());
    EXPECT_FALSE(message->has_value());
    unblocked.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(unblocked.load());
  a->Close();
  receiver.join();
  EXPECT_TRUE(unblocked.load());
}

TEST(WebSocketPairTest, CloseUnblocksASenderStalledOnBackpressure) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  constexpr std::size_t kDepth = 8;
  for (std::size_t i = 0; i < kDepth; ++i) {
    ASSERT_TRUE(a->Send(Text("n", std::to_string(i))).ok());
  }
  std::atomic<bool> failed{false};
  std::thread sender([&a, &failed] {
    const auto outcome = a->Send(Text("n", "overflow"));  // blocks on the full queue
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);
    failed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(failed.load());
  b->Close();
  sender.join();
  EXPECT_TRUE(failed.load());
}

TEST(WebSocketPairTest, AMessageTheCodecRefusesFailsValidationAndSparesTheSession) {
  auto [a, b] = InMemoryWebSocketPair::Create();
  const auto refused = a->Send(Message{.headers = {{"", true}}});  // empty header name
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // Wire-transport parity: the session is untouched and stays usable.
  ASSERT_TRUE(a->Send(Text("chat", "still alive")).ok());
  auto message = b->Receive();
  ASSERT_TRUE(message.ok() && message->has_value());
  EXPECT_EQ((*message)->payload.ToString(), "still alive");
}

}  // namespace
}  // namespace smithy::http
