#include "smithy/http/websocket_pair.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "smithy/eventstream/frame.h"

namespace smithy::http {
namespace {

// The Beast session's receive-side buffer bound (beast_transport.cc): a
// stalled receiver blocks its sender after this many queued messages.
constexpr std::size_t kQueueDepth = 8;

// The session both ends share: one queue per direction, one session-wide
// closed flag (either end's Close ends the session for both), one mutex and
// condition variable for all four blocking call sites — contention is
// bounded by the contract's one sender + one receiver per end.
struct PairState {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::deque<eventstream::Message>, 2> queues;
  bool closed = false;
};

class PairEnd final : public WebSocket {
 public:
  PairEnd(std::shared_ptr<PairState> state, std::size_t send_index)
      : state_(std::move(state)), send_index_(send_index) {}

  Outcome<std::optional<eventstream::Message>> Receive() override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    std::deque<eventstream::Message>& inbound = state_->queues[1 - send_index_];
    state_->changed.wait(lock, [&] { return !inbound.empty() || state_->closed; });
    if (inbound.empty()) {
      return std::optional<eventstream::Message>();  // the stream's clean end
    }
    // Messages queued before a close still belong to the application, in
    // order (the wire session's drain behavior).
    eventstream::Message message = std::move(inbound.front());
    inbound.pop_front();
    state_->changed.notify_all();  // wake a sender blocked on the bound
    return std::optional<eventstream::Message>(std::move(message));
  }

  Outcome<Unit> Send(const eventstream::Message& message) override {
    // Encode-validation parity with the wire transports: what the codec
    // refuses never enters the session, and the session stays usable. The
    // bytes themselves are dropped — the peer receives the message value,
    // which the codec's round-trip guarantees is the same thing.
    if (auto frame = eventstream::EncodeMessage(message); !frame.ok()) {
      return std::move(frame).error();  // the codec's Validation, verbatim
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    std::deque<eventstream::Message>& outbound = state_->queues[send_index_];
    state_->changed.wait(lock, [&] { return outbound.size() < kQueueDepth || state_->closed; });
    if (state_->closed) {
      return Error::Transport("websocket pair: session is closed");
    }
    outbound.push_back(message);
    state_->changed.notify_all();
    return Unit{};
  }

  void Close() override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    state_->changed.notify_all();
  }

 private:
  std::shared_ptr<PairState> state_;
  std::size_t send_index_;
};

}  // namespace

std::pair<std::shared_ptr<WebSocket>, std::shared_ptr<WebSocket>> InMemoryWebSocketPair::Create() {
  auto state = std::make_shared<PairState>();
  return {std::make_shared<PairEnd>(state, 0), std::make_shared<PairEnd>(state, 1)};
}

}  // namespace smithy::http
