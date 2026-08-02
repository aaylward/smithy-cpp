#ifndef SMITHY_TESTING_WEBSOCKET_CONTRACT_TEST_H_
#define SMITHY_TESTING_WEBSOCKET_CONTRACT_TEST_H_

// The transport-neutral half of the ADR-0019 WebSocket contract, written
// once and run against every implementation (issue #173).
//
// The two in-repo transports had the same terminal-ordering bug at the same
// time, in code that reads nothing alike — because their suites mirror each
// other by hand, and a rule nobody restated in the second file is a rule the
// second file does not check. Anything an implementation must do REGARDLESS
// of its wire belongs here instead; the transport suites keep what is
// genuinely theirs (close codes, wire framing, handshake, TLS, error
// wording).
//
// An implementation joins by defining a driver and instantiating:
//
//   INSTANTIATE_TYPED_TEST_SUITE_P(MyTransport, WebSocketContractTest,
//                                  MyDriver);
//
// The driver stands up one session whose PEER NEVER READS, so a bounded
// number of sends wedges the wire and the next one parks — the state these
// tests need and the one thing only the transport knows how to reach:
//
//   struct Driver {
//     // The implementation under test. Called once, may block until the
//     // session exists.
//     std::shared_ptr<smithy::http::WebSocket> Socket();
//
//     // Payload for wedge attempt `n` — sized so at most kWedgeAttempts of
//     // them back the wire up.
//     smithy::eventstream::Message BulkMessage(int n);
//     static constexpr int kWedgeAttempts = ...;
//
//     // Drives a terminal transition from the far side (a peer close, a
//     // protocol violation, a reset — whichever this transport can produce
//     // while a write is wedged). May block for the whole teardown: the
//     // suite always calls it on its own thread.
//     void EndSessionFromPeer();
//   };

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "smithy/core/outcome.h"
#include "smithy/eventstream/async_event_stream.h"
#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::testing {

// A one-shot deadlined handoff. Deadlined rather than infinite because the
// regressions these tests catch are deadlocks: a plain wait would hang out
// the whole bazel timeout with nothing to read, and on the transports whose
// teardown runs on the caller's thread the wedged thread cannot even be
// joined. Aborting names the test that wedged and stops.
template <typename T>
class ContractMailbox {
 public:
  void Post(T value) {
    // Notify under the lock, not after it. These mailboxes live on test
    // frames and the poster is a wire thread, so a notify outside the lock
    // races the waiter's return: Wait() re-checks the predicate, returns,
    // the frame unwinds, and ~condition_variable runs while the poster is
    // still inside notify_all. Holding the lock keeps the waiter parked on
    // the mutex until the notify is done.
    const std::lock_guard<std::mutex> lock(mutex_);
    ASSERT_FALSE(value_.has_value()) << "completion fired twice";
    value_.emplace(std::move(value));
    ready_.notify_all();
  }

  bool WaitFor(std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_.wait_for(lock, budget, [this] { return value_.has_value(); });
  }

  T Wait() {
    if (!WaitFor(std::chrono::seconds(30))) {
      ADD_FAILURE() << "contract mailbox: no completion arrived within the deadline";
      std::abort();  // T (an Outcome) has no default value to limp on with
    }
    const std::lock_guard<std::mutex> lock(mutex_);
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

// The session under test, typed as raw Messages: these contracts are about
// parking and completion, so the codecs stay out of the way.
using ContractStream = eventstream::AsyncEventStream<eventstream::Message, eventstream::Message>;
using ContractHandle = eventstream::EventStreamHandle<eventstream::Message>;

inline Outcome<eventstream::Message> IdentityCodec(const eventstream::Message& message) {
  return message;
}

// Issues sends through `issue` until one fails to complete: that one is
// parked on the wedged wire — and, when `issue` goes through a handle, it
// is holding an ADR-0017 revocation pin. Returns the parked attempt's
// mailbox (still unfired) so the caller can wait on that exact completion,
// or null if the wire never wedged.
//
// The mailboxes are shared_ptr because the completions outlive this frame:
// the parked one fires when the session ends, and a caller that never reads
// it still must not leave the callback writing into a dead mailbox.
template <typename Driver, typename Issue>
std::shared_ptr<ContractMailbox<Outcome<Unit>>> WedgeThenPark(Driver& driver, Issue&& issue) {
  for (int i = 0; i < Driver::kWedgeAttempts; ++i) {
    auto attempt = std::make_shared<ContractMailbox<Outcome<Unit>>>();
    issue(driver.BulkMessage(i), [attempt](Outcome<Unit> sent) { attempt->Post(std::move(sent)); });
    if (!attempt->WaitFor(std::chrono::seconds(2))) {
      return attempt;  // parked: the wire took what it could and stopped
    }
    if (!attempt->Wait().ok()) return nullptr;  // the wire died early
  }
  return nullptr;
}

template <typename Driver>
class WebSocketContractTest : public ::testing::Test {};

TYPED_TEST_SUITE_P(WebSocketContractTest);

// The issue-#173 regression, on every transport. A session ends with BOTH
// classes parked: the loop's receive, and a handle send holding a
// revocation pin. Completing the receive first resumes the loop, ends it,
// and runs ~AsyncEventStream's pin drain inline — which can only be
// released by the send completion the transition is still holding, one
// frame below, on the thread that is now blocked. Sends first, or the
// teardown never returns.
TYPED_TEST_P(WebSocketContractTest, ATerminalTransitionFiresTheParkedSendBeforeTheParkedReceive) {
  TypeParam driver;
  ContractMailbox<ContractHandle> minted;
  ContractMailbox<Unit> torn_down;
  std::atomic<bool> loop_ended{false};

  [](std::shared_ptr<http::WebSocket> socket, ContractMailbox<ContractHandle>* minted,
     std::atomic<bool>* ended) -> eventstream::Detached {
    ContractStream stream(std::move(socket), IdentityCodec, IdentityCodec);
    minted->Post(stream.Share());
    (void)co_await stream.Receive();  // parked until the session ends
    *ended = true;
  }(driver.Socket(), &minted, &loop_ended);

  ContractHandle handle = minted.Wait();
  auto parked =
      WedgeThenPark(driver, [&handle](const eventstream::Message& message, auto callback) {
        handle.SendAsync(message, std::move(callback));
      });
  ASSERT_NE(parked, nullptr) << "the wire never wedged; check the driver";

  // On transports whose teardown runs on the caller's thread, this call is
  // what deadlocks; keep it off the test thread so the failure is legible.
  std::thread ender([&] {
    driver.EndSessionFromPeer();
    torn_down.Post(Unit{});
  });
  torn_down.Wait();  // a wedged teardown aborts here, before the join
  ender.join();      // nothing outlives the mailboxes on this frame

  auto dead = parked->Wait();
  ASSERT_FALSE(dead.ok()) << "a send on an ended session must fail";
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
  for (int i = 0; i < 1000 && !loop_ended.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(loop_ended.load()) << "the session loop never unwound";
}

// The same transition with only a receive parked: the send-first ordering
// must not have made the receive conditional on there being a send.
TYPED_TEST_P(WebSocketContractTest, ATerminalTransitionCompletesALoneParkedReceive) {
  TypeParam driver;
  ContractMailbox<Unit> torn_down;
  std::atomic<bool> loop_ended{false};

  [](std::shared_ptr<http::WebSocket> socket, std::atomic<bool>* ended) -> eventstream::Detached {
    ContractStream stream(std::move(socket), IdentityCodec, IdentityCodec);
    (void)co_await stream.Receive();
    *ended = true;
  }(driver.Socket(), &loop_ended);

  std::thread ender([&] {
    driver.EndSessionFromPeer();
    torn_down.Post(Unit{});
  });
  torn_down.Wait();
  ender.join();

  for (int i = 0; i < 1000 && !loop_ended.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(loop_ended.load()) << "a lone parked receive was left hanging";
}

// One outstanding send-class operation per session: the second refuses
// inline with Validation rather than queueing behind the first.
TYPED_TEST_P(WebSocketContractTest, ASecondSendClassOperationRefusesWhileOneIsParked) {
  TypeParam driver;
  std::shared_ptr<http::WebSocket> socket = driver.Socket();
  ContractMailbox<Outcome<Unit>> refused;

  auto parked =
      WedgeThenPark(driver, [&socket](const eventstream::Message& message, auto callback) {
        socket->SendAsync(message, std::move(callback));
      });
  ASSERT_NE(parked, nullptr) << "the wire never wedged; check the driver";

  socket->SendAsync(driver.BulkMessage(0),
                    [&refused](Outcome<Unit> sent) { refused.Post(std::move(sent)); });
  auto refusal = refused.Wait();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  std::thread ender([&] { driver.EndSessionFromPeer(); });
  ender.join();
}

// One outstanding receive-class operation per session, same shape.
TYPED_TEST_P(WebSocketContractTest, ASecondReceiveClassOperationRefusesWhileOneIsParked) {
  TypeParam driver;
  std::shared_ptr<http::WebSocket> socket = driver.Socket();
  ContractMailbox<Outcome<std::optional<eventstream::Message>>> first;
  ContractMailbox<Outcome<std::optional<eventstream::Message>>> second;

  socket->ReceiveAsync(
      [&first](Outcome<std::optional<eventstream::Message>> got) { first.Post(std::move(got)); });
  ASSERT_TRUE(first.Empty()) << "the first receive should park on a quiet wire";

  socket->ReceiveAsync(
      [&second](Outcome<std::optional<eventstream::Message>> got) { second.Post(std::move(got)); });
  auto refusal = second.Wait();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  // The parked first one still belongs to the session: ending it completes.
  std::thread ender([&] { driver.EndSessionFromPeer(); });
  ender.join();
  (void)first.Wait();
}

REGISTER_TYPED_TEST_SUITE_P(WebSocketContractTest,
                            ATerminalTransitionFiresTheParkedSendBeforeTheParkedReceive,
                            ATerminalTransitionCompletesALoneParkedReceive,
                            ASecondSendClassOperationRefusesWhileOneIsParked,
                            ASecondReceiveClassOperationRefusesWhileOneIsParked);

}  // namespace smithy::testing

#endif  // SMITHY_TESTING_WEBSOCKET_CONTRACT_TEST_H_
