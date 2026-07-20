#include "smithy/http/websocket_pair.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "smithy/eventstream/frame.h"

namespace smithy::http {
namespace {

// The Beast session's receive-side buffer bound (beast_transport.cc),
// published on the class so backpressure tests derive from it.
constexpr std::size_t kQueueDepth = InMemoryWebSocketPair::kQueueDepth;

// A parked SendAsync: the message it will queue and the completion to fire.
struct PendingSend {
  eventstream::Message message;
  WebSocket::SendCallback callback;
};

// The session both ends share: one queue per direction, one session-wide
// closed flag (either end's Close ends the session for both), one mutex and
// condition variable for all four blocking call sites — contention is
// bounded by the contract's one sender + one receiver per end. The ADR-0019
// async slots park at most one receive and one send per end; completions
// fire after the lock is released, on whichever peer thread completed the
// operation (the pair has no executor to post to — unlike the wire
// transports, a synchronous ping-pong of inline completions recurses, so
// keep coroutine test volleys modest).
struct PairState {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::deque<eventstream::Message>, 2> queues;
  // Indexed by the owning end's send index: an end's parked receive, its
  // parked send, and its counts of receivers/senders blocked in the
  // blocking calls (the async twin refuses while one waits, and a blocking
  // call waits behind a parked async op — the one-outstanding contract's
  // two halves, same shape as the Beast session).
  std::array<WebSocket::ReceiveCallback, 2> pending_receive;
  std::array<std::optional<PendingSend>, 2> pending_send;
  std::array<int, 2> blocked_receivers{};
  std::array<int, 2> blocked_senders{};
  bool closed = false;
};

class PairEnd final : public WebSocket {
 public:
  PairEnd(std::shared_ptr<PairState> state, std::size_t send_index)
      : state_(std::move(state)), send_index_(send_index) {}

  Outcome<std::optional<eventstream::Message>> Receive() override {
    WebSocket::SendCallback absorbed;
    Outcome<std::optional<eventstream::Message>> result = std::optional<eventstream::Message>();
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      std::deque<eventstream::Message>& inbound = state_->queues[1 - send_index_];
      // A parked async receive owns the next arrival (the peer's send hands
      // it over directly), so a blocking receiver waits behind it — the
      // serialize-by-waiting half of the one-outstanding contract.
      ++state_->blocked_receivers[send_index_];
      state_->changed.wait(lock, [&] {
        return (!inbound.empty() && !state_->pending_receive[send_index_]) || state_->closed;
      });
      --state_->blocked_receivers[send_index_];
      if (inbound.empty()) {
        return std::optional<eventstream::Message>();  // the stream's clean end
      }
      // Messages queued before a close still belong to the application, in
      // order (the wire session's drain behavior).
      eventstream::Message message = std::move(inbound.front());
      inbound.pop_front();
      absorbed = AbsorbPeerPendingSendLocked();
      state_->changed.notify_all();  // wake a sender blocked on the bound
      result = std::optional<eventstream::Message>(std::move(message));
    }
    if (absorbed) absorbed(Unit{});
    return result;
  }

  Outcome<Unit> Send(const eventstream::Message& message) override {
    // Encode-validation parity with the wire transports: what the codec
    // refuses never enters the session, and the session stays usable. The
    // bytes themselves are dropped — the peer receives the message value,
    // which the codec's round-trip guarantees is the same thing.
    if (auto frame = eventstream::EncodeMessage(message); !frame.ok()) {
      return std::move(frame).error();  // the codec's Validation, verbatim
    }
    WebSocket::ReceiveCallback deliver;
    eventstream::Message delivered;
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      std::deque<eventstream::Message>& outbound = state_->queues[send_index_];
      // A parked async send goes first (FIFO across both send APIs).
      ++state_->blocked_senders[send_index_];
      state_->changed.wait(lock, [&] {
        return (outbound.size() < kQueueDepth && !state_->pending_send[send_index_]) ||
               state_->closed;
      });
      --state_->blocked_senders[send_index_];
      if (state_->closed) {
        return Error::Transport("websocket pair: session is closed");
      }
      deliver = TakePeerReceiverLocked(message, delivered);
      if (!deliver) {
        outbound.push_back(message);
        state_->changed.notify_all();
      }
    }
    if (deliver) deliver(std::optional<eventstream::Message>(std::move(delivered)));
    return Unit{};
  }

  void Close() override {
    std::array<WebSocket::ReceiveCallback, 2> receives;
    std::array<WebSocket::SendCallback, 2> sends;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      state_->closed = true;
      for (std::size_t end = 0; end < 2; ++end) {
        // std::exchange, never a bare std::move: libc++'s small-buffer
        // std::function move leaves the source engaged, and these slots'
        // emptiness is the one-outstanding busy signal.
        receives[end] = std::exchange(state_->pending_receive[end], nullptr);
        std::optional<PendingSend>& parked = state_->pending_send[end];
        if (parked.has_value()) {
          sends[end] = std::move(parked->callback);
          parked.reset();
        }
      }
      state_->changed.notify_all();
    }
    // A parked receive implies its queue was empty (any push completes it
    // immediately), so the clean end is the honest outcome.
    for (auto& receive : receives) {
      if (receive) receive(std::optional<eventstream::Message>());
    }
    for (auto& send : sends) {
      if (send) send(Error::Transport("websocket pair: session is closed"));
    }
  }

  void ReceiveAsync(WebSocket::ReceiveCallback callback) override {
    WebSocket::SendCallback absorbed;
    Outcome<std::optional<eventstream::Message>> immediate = std::optional<eventstream::Message>();
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->pending_receive[send_index_] || state_->blocked_receivers[send_index_] > 0) {
        callback(Error::Validation("websocket pair: a receive is already outstanding"));
        return;
      }
      std::deque<eventstream::Message>& inbound = state_->queues[1 - send_index_];
      if (!inbound.empty()) {
        eventstream::Message message = std::move(inbound.front());
        inbound.pop_front();
        absorbed = AbsorbPeerPendingSendLocked();
        state_->changed.notify_all();
        immediate = std::optional<eventstream::Message>(std::move(message));
      } else if (state_->closed) {
        immediate = std::optional<eventstream::Message>();
      } else {
        state_->pending_receive[send_index_] = std::move(callback);
        return;  // a send or the close completes it
      }
    }
    if (absorbed) absorbed(Unit{});
    callback(std::move(immediate));
  }

  void SendAsync(const eventstream::Message& message, WebSocket::SendCallback callback) override {
    if (auto frame = eventstream::EncodeMessage(message); !frame.ok()) {
      callback(std::move(frame).error());  // the codec's Validation, inline
      return;
    }
    WebSocket::ReceiveCallback deliver;
    eventstream::Message delivered;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->closed) {
        callback(Error::Transport("websocket pair: session is closed"));
        return;
      }
      if (state_->pending_send[send_index_].has_value() ||
          state_->blocked_senders[send_index_] > 0) {
        callback(Error::Validation("websocket pair: a send is already in flight"));
        return;
      }
      std::deque<eventstream::Message>& outbound = state_->queues[send_index_];
      if (outbound.size() >= kQueueDepth) {
        // Backpressure without blocking: park until the receiver drains.
        state_->pending_send[send_index_] = PendingSend{message, std::move(callback)};
        return;
      }
      deliver = TakePeerReceiverLocked(message, delivered);
      if (!deliver) {
        outbound.push_back(message);
        state_->changed.notify_all();
      }
    }
    if (deliver) deliver(std::optional<eventstream::Message>(std::move(delivered)));
    callback(Unit{});
  }

  bool SupportsAsync() const override { return true; }

 private:
  // With the lock held: if the peer parked an async receive, hand it this
  // message directly (its queue is empty by the parked-receive invariant);
  // the caller fires the returned callback after unlocking.
  WebSocket::ReceiveCallback TakePeerReceiverLocked(const eventstream::Message& message,
                                                    eventstream::Message& delivered) {
    WebSocket::ReceiveCallback& parked = state_->pending_receive[1 - send_index_];
    if (!parked) return nullptr;
    delivered = message;
    return std::exchange(parked, nullptr);
  }

  // With the lock held: after this end freed queue space by receiving, the
  // peer's parked async send (if any) takes the slot — FIFO order for its
  // direction. Returns its completion for the caller to fire.
  WebSocket::SendCallback AbsorbPeerPendingSendLocked() {
    std::optional<PendingSend>& parked = state_->pending_send[1 - send_index_];
    if (!parked.has_value() || state_->queues[1 - send_index_].size() >= kQueueDepth) {
      return nullptr;
    }
    state_->queues[1 - send_index_].push_back(std::move(parked->message));
    WebSocket::SendCallback callback = std::move(parked->callback);
    parked.reset();
    return callback;
  }

  std::shared_ptr<PairState> state_;
  std::size_t send_index_;
};

}  // namespace

std::pair<std::shared_ptr<WebSocket>, std::shared_ptr<WebSocket>> InMemoryWebSocketPair::Create() {
  auto state = std::make_shared<PairState>();
  return {std::make_shared<PairEnd>(state, 0), std::make_shared<PairEnd>(state, 1)};
}

}  // namespace smithy::http
