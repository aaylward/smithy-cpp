#ifndef SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_
#define SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "smithy/core/outcome.h"
#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::eventstream {

// The session-loop coroutine type (ADR-0019): fire and forget. A Detached
// coroutine starts eagerly, owns nothing after launch (its frame frees
// itself at the end), and contains unhandled exceptions to a log line —
// the transport's containment posture, since these loops run on wire
// threads. The whole coroutine surface this slice ships is Detached plus
// AsyncEventStream's two awaitables; a general task type can grow later
// without disturbing either.
struct Detached {
  struct promise_type {
    // NOLINTBEGIN(readability-convert-member-functions-to-static) — the
    // coroutine machinery calls these through the promise instance, and
    // making them static just trips static-accessed-through-instance at
    // every coroutine that uses the type.
    Detached get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& e) {
        std::clog << "smithy: detached stream coroutine threw: " << e.what() << "\n";
      } catch (...) {
        std::clog << "smithy: detached stream coroutine threw a non-std exception\n";
      }
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
  };
};

// The typed session's coroutine adapter (ADR-0019): EventStream's contract
// over the completion-driven socket primitives, with `co_await` where the
// blocking facade parks a thread. Owns its session (shared_ptr — the async
// session's natural shape, per ADR-0016's forecast); typically it lives in
// the frame of the Detached loop serving it, so the session ends when the
// loop does.
//
//   smithy::eventstream::Detached Serve(AsyncEventStream<Tx, Rx> stream) {
//     while (true) {
//       auto event = co_await stream.Receive();
//       if (!event.ok() || !event->has_value()) break;
//       ... co_await stream.Send(reply) ...
//     }
//   }
//
// Resumption runs on the transport's completion context (a Beast io
// thread; the peer's thread on the in-memory pair): never block there —
// blocking work belongs on application threads, reached through Share().
// The socket's one-outstanding contracts pass through: one co_await
// Receive and one co_await Send at a time, which a single sequential
// coroutine satisfies by construction. Close() from any thread completes
// an outstanding await with the stream's terminal outcome — cancellation
// stays "close the session".
//
// Two lifetime rules for the Detached pattern: (1) everything the
// coroutine references (registry, hub state) must outlive the transport —
// the loops resume on io threads long after the launch callback returned.
// (2) This destructor revokes Share() handles and WAITS for any pinned
// handle operation to drain (ADR-0017); at the end of a Detached loop
// that wait runs on the completion context, and the completion it waits
// for needs an io thread — so leave transports that mix Detached sessions
// with handle traffic more than one io thread (the Beast default is 4).
template <typename Tx, typename Rx>
class AsyncEventStream {
 public:
  using Encoder = std::function<Outcome<Message>(const Tx&)>;
  using Decoder = std::function<Outcome<Rx>(const Message&)>;

  AsyncEventStream(std::shared_ptr<http::WebSocket> socket, Encoder encode, Decoder decode)
      : socket_(std::move(socket)), encode_(std::move(encode)), decode_(std::move(decode)) {}

  // Move-only, like EventStream: the view owner revokes on destruction, so
  // handles minted by Share() fail softly once the stream is gone.
  ~AsyncEventStream() = default;
  AsyncEventStream(AsyncEventStream&&) noexcept = default;
  // No explicit noexcept: the computed one depends on std::function's
  // move-assignment, which the standard does not make noexcept.
  AsyncEventStream& operator=(AsyncEventStream&&) = default;
  AsyncEventStream(const AsyncEventStream&) = delete;
  AsyncEventStream& operator=(const AsyncEventStream&) = delete;

  // Awaits the next event: nullopt is the peer's clean close. A message
  // the decoder rejects — a received exception (decoded to its modeled
  // Error) or an undecodable message — is terminal (ADR-0016): the session
  // is closed and the error is the awaited result.
  class [[nodiscard]] ReceiveAwaitable {
   public:
    explicit ReceiveAwaitable(AsyncEventStream* stream) : stream_(stream) {}
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> coroutine) {
      // The completion may fire before this returns (immediate refusals,
      // the in-memory pair, a ready queue whose post beats us): whichever
      // side arrives second owns the resume. A synchronous completion
      // resumes by returning false — flattening would-be recursion into
      // iteration and never touching the frame from two threads at once —
      // and only a truly asynchronous completion resumes from the callback.
      // The flag can live in the awaitable: the frame dies only after a
      // resume, and every resume is sequenced after both exchanges.
      stream_->socket_->ReceiveAsync([this, coroutine](Outcome<std::optional<Message>> message) {
        raw_ = std::move(message);
        if (arrived_.exchange(true)) coroutine.resume();
      });
      return !arrived_.exchange(true);  // suspend iff the callback has not run
    }
    Outcome<std::optional<Rx>> await_resume() {
      if (!raw_.ok()) return std::move(raw_).error();
      std::optional<Message>& message = *raw_;
      if (!message.has_value()) return std::optional<Rx>();
      auto event = stream_->decode_(*message);
      if (!event.ok()) {
        stream_->Close();
        return std::move(event).error();
      }
      return std::optional<Rx>(std::move(*event));
    }

   private:
    AsyncEventStream* stream_;
    std::atomic<bool> arrived_{false};
    // Outcome has no default constructor; the placeholder is overwritten
    // before any resume. NOLINT(readability-redundant-member-init)
    Outcome<std::optional<Message>> raw_ = std::optional<Message>();  // NOLINT
  };

  ReceiveAwaitable Receive() { return ReceiveAwaitable(this); }

  // Awaits one event onto the wire. Encoder failures surface without
  // suspending and leave the session untouched; wire failures are the
  // socket's (Error::Transport once the session ended). Uncallable when Tx
  // is NoEvents, like every send in this direction.
  class [[nodiscard]] SendAwaitable {
   public:
    SendAwaitable(AsyncEventStream* stream, Outcome<Message> message)
        : stream_(stream), message_(std::move(message)) {}
    bool await_ready() const noexcept { return !message_.ok(); }
    bool await_suspend(std::coroutine_handle<> coroutine) {
      // Same second-arrival-resumes race as ReceiveAwaitable.
      stream_->socket_->SendAsync(*message_, [this, coroutine](Outcome<Unit> sent) {
        sent_ = std::move(sent);
        if (arrived_.exchange(true)) coroutine.resume();
      });
      return !arrived_.exchange(true);  // suspend iff the callback has not run
    }
    Outcome<Unit> await_resume() {
      if (!message_.ok()) return std::move(message_).error();
      return std::move(sent_);
    }

   private:
    AsyncEventStream* stream_;
    std::atomic<bool> arrived_{false};
    Outcome<Message> message_;
    Outcome<Unit> sent_ = Unit{};
  };

  SendAwaitable Send(const Tx& event) {
    static_assert(!std::is_same_v<Tx, NoEvents>,
                  "this stream models no events in this direction: Send is not callable on a "
                  "receive-only AsyncEventStream (use Receive/Close)");
    return SendAwaitable(this, encode_(event));
  }

  // Initiates the close handshake; idempotent and safe from any thread.
  // An outstanding await completes with the terminal outcome.
  void Close() { socket_->Close(); }

  // Whether the wrapped session implements the async primitives — false
  // means every co_await completes immediately with the base default's
  // polite refusal (websocket.h); check before adopting a socket of
  // unknown provenance.
  bool SupportsAsync() const { return socket_->SupportsAsync(); }

  // The same owning handle EventStream mints (ADR-0017), over the same
  // revocable view: blocking or async sends from any thread, safe to hold
  // beyond this stream's lifetime, and the SessionRegistry composes
  // unchanged. The handle never extends the session — destroying this
  // stream after a Share() closes it, waits out pinned operations, and
  // leaves every handle failing softly.
  EventStreamHandle<Tx> Share() {
    return EventStreamHandle<Tx>(view_.Ensure(socket_.get()), encode_);
  }

 private:
  std::shared_ptr<http::WebSocket> socket_;
  Encoder encode_;
  Decoder decode_;
  // Last member on purpose: its teardown (End) runs first and needs the
  // socket above still alive.
  internal::SharedViewOwner view_;
};

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_
