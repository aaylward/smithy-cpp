#ifndef SMITHY_EVENTSTREAM_EVENT_STREAM_H_
#define SMITHY_EVENTSTREAM_EVENT_STREAM_H_

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "smithy/core/outcome.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::eventstream {

// The vacant direction of a one-directional stream (ADR-0016): when an
// operation models no event union for a direction, generated code
// parameterizes that direction's EventStream slot with NoEvents. Nothing
// ever constructs one, so the direction's encode function is never invoked;
// a message received on a NoEvents direction is undecodable and therefore
// terminal (the generated decoder rejects it).
struct NoEvents {
  friend bool operator==(const NoEvents&, const NoEvents&) = default;
};

// The typed event-stream session (ADR-0016): one WebSocket plus the two
// codec functions generated code supplies. Tx is what this end sends, Rx
// what it receives — a client and its server hold the same session with
// the parameters swapped. EventStream only plumbs: the encoder turns a
// typed event into a message, the decoder turns a message into the typed
// event or into the modeled Error for an exception message; the envelope
// convention lives in the codecs (smithy/eventstream/envelope.h), never
// here.
//
// Blocking, full duplex, backpressure, and cancellation are the wrapped
// WebSocket's contract verbatim: one sending and one receiving thread may
// block concurrently, and Close() from any thread is how either is
// unblocked.
template <typename Tx, typename Rx>
class EventStream {
 public:
  using Encoder = std::function<Outcome<Message>(const Tx&)>;
  using Decoder = std::function<Outcome<Rx>(const Message&)>;

  // Shares ownership of the socket — the client path
  // (BeastWebSocketClient::Dial's handle).
  EventStream(std::shared_ptr<http::WebSocket> socket, Encoder encode, Decoder decode)
      : owned_(std::move(socket)),
        socket_(owned_.get()),
        encode_(std::move(encode)),
        decode_(std::move(decode)) {}

  // Borrows the socket — the server path, where on_websocket lends a
  // WebSocket& that stays valid until the serve callback returns; the
  // stream must not outlive that borrow.
  EventStream(http::WebSocket& socket, Encoder encode, Decoder decode)
      : socket_(&socket), encode_(std::move(encode)), decode_(std::move(decode)) {}

  // Encodes and sends one event, blocking until its frame is on the wire.
  // An encoder failure surfaces as-is and leaves the session untouched;
  // wire failures are the WebSocket's (Error::Transport once closed).
  Outcome<Unit> Send(const Tx& event) {
    auto message = encode_(event);
    if (!message.ok()) return std::move(message).error();
    return socket_->Send(*message);
  }

  // Blocks for the next event. nullopt is the peer's clean close — the
  // stream's natural end. A message the decoder rejects — a received
  // exception (the decoder returns it as the modeled Error) or an
  // undecodable message — is terminal (ADR-0016): the session is closed
  // and the error returned.
  Outcome<std::optional<Rx>> Receive() {
    auto message = socket_->Receive();
    if (!message.ok()) return std::move(message).error();
    if (!message->has_value()) return std::optional<Rx>();
    auto event = decode_(**message);
    if (!event.ok()) {
      Close();
      return std::move(event).error();
    }
    return std::optional<Rx>(std::move(*event));
  }

  // Initiates the close handshake; idempotent and safe from any thread
  // (the WebSocket contract).
  void Close() { socket_->Close(); }

 private:
  std::shared_ptr<http::WebSocket> owned_;  // null on the borrowed path
  http::WebSocket* socket_;
  Encoder encode_;
  Decoder decode_;
};

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_EVENT_STREAM_H_
