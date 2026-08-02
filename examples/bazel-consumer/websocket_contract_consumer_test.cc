// Out-of-tree proof for the ADR-0019 implementor contract (#173): a
// WebSocket written entirely in consumer code adopts the async primitives,
// runs its terminal transition through WebSocket::TerminalWaiters, and is
// held to the same shared contract suite the in-repo transports are.
//
// This is what makes the seam's claim real rather than aspirational. The
// runtime says the async methods are public virtuals a third party may
// override, and that overriding them accepts the send-before-receive rule;
// this test is the only place that a third party actually does so — across
// the module boundary, against the published targets alone, with no
// in-repo transport in the loop.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/websocket_contract_test.h"

namespace {

using smithy::Outcome;
using smithy::Unit;
using smithy::eventstream::Message;
using smithy::http::WebSocket;

// A third-party session: a bounded outbound wire nobody drains, one parked
// receive, one parked send. Deliberately minimal — the point is not the
// wire but that the terminal transition is expressed with TerminalWaiters,
// so this implementation inherits the ordering rule without its author
// having to rediscover why the rule exists.
class ConsumerSocket final : public WebSocket {
 public:
  // Small on purpose: the contract suite wedges the wire by sending, and a
  // shallow queue gets there in a few messages.
  static constexpr std::size_t kDepth = 4;

  Outcome<std::optional<Message>> Receive() override {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return closed_; });
    return std::optional<Message>();  // this socket's peer only ever ends it
  }

  Outcome<std::optional<Message>> Receive(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [this] { return closed_; })) {
      return smithy::Error::Timeout("consumer socket: no message within the deadline");
    }
    return std::optional<Message>();
  }

  Outcome<Unit> Send(const Message& message) override {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return queued_ < kDepth || closed_; });
    if (closed_) return smithy::Error::Transport("consumer socket: session is closed");
    ++queued_;
    (void)message;
    return Unit{};
  }

  void Close() override { EndSession(); }

  bool SupportsAsync() const override { return true; }

  // Both async twins: park under the lock, or complete inline once the
  // lock is released — the seam's documented shapes, nothing more.
  void ReceiveAsync(ReceiveCallback callback) override {
    Outcome<std::optional<Message>> immediate = std::optional<Message>();  // the clean end
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!closed_ && !pending_receive_) {
        pending_receive_ = std::move(callback);
        return;  // EndSession completes it
      }
      if (!closed_) {
        immediate = smithy::Error::Validation("consumer socket: a receive is already outstanding");
      }
    }
    callback(std::move(immediate));
  }

  void SendAsync(const Message& message, SendCallback callback) override {
    (void)message;
    Outcome<Unit> immediate = Unit{};
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        immediate = smithy::Error::Transport("consumer socket: session is closed");
      } else if (pending_send_) {
        immediate = smithy::Error::Validation("consumer socket: a send is already in flight");
      } else if (queued_ >= kDepth) {
        pending_send_ = std::move(callback);  // parked on the full wire
        return;
      } else {
        ++queued_;
      }
    }
    callback(std::move(immediate));
  }

  // The far side ending the session — a peer close, a reset, whatever this
  // implementation's wire calls it. Takes both parked completions under the
  // lock, releases it, and fires through TerminalWaiters, which is what
  // puts the send ahead of the receive.
  void EndSession() {
    WebSocket::TerminalWaiters waiters;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      closed_ = true;
      waiters = WebSocket::TerminalWaiters(std::exchange(pending_receive_, nullptr),
                                           std::exchange(pending_send_, nullptr));
      changed_.notify_all();
    }
    std::move(waiters).Fire(
        smithy::Error::Transport("consumer socket: session is closed"), std::optional<Message>(),
        [](const char*, const auto& callback, auto outcome) { callback(std::move(outcome)); });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  ReceiveCallback pending_receive_;
  SendCallback pending_send_;
  std::size_t queued_ = 0;
  bool closed_ = false;
};

struct ConsumerContractDriver {
  static constexpr int kWedgeAttempts = static_cast<int>(ConsumerSocket::kDepth) + 4;

  std::shared_ptr<WebSocket> Socket() { return socket_; }

  Message BulkMessage(int n) {
    return Message{.headers = {{":event-type", "bulk"}},
                   .payload = smithy::Blob::FromString(std::to_string(n))};
  }

  void EndSessionFromPeer() { socket_->EndSession(); }

  std::shared_ptr<ConsumerSocket> socket_ = std::make_shared<ConsumerSocket>();
};

}  // namespace

// gtest builds the registration symbols from the bare suite name, so the
// instantiation lives in the namespace the suite was registered in.
namespace smithy::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(ConsumerSocket, WebSocketContractTest, ConsumerContractDriver);
}  // namespace smithy::testing
