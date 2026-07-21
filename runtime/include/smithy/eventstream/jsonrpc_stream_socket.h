#ifndef SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_
#define SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_

#include <memory>
#include <optional>

#include "smithy/core/document.h"
#include "smithy/core/outcome.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::eventstream {

// The jsonRpc2 stream translation (ADR-0023), worn as a delegating
// WebSocket: above it, EventStream/AsyncEventStream and generated code
// trade the same envelope-bearing Messages every wire carries; below it,
// headerless raw-text Messages whose payload is one JSON-RPC 2.0 envelope
// per frame. Unlike ADR-0018's transport-internal mode, the wrapper runs
// above the socket — both ends wear one, on both seams, over Beast (whose
// raw-text mode carries the payloads as verbatim text frames) AND the
// in-memory pair (which carries Message values unchanged) — so every
// integration test exercises the actual JSON text.
//
// Sends translate events into notifications echoing the opening call's id;
// exception messages are refused (the generated serve path mints terminal
// envelopes — it owns the @httpError table). Receives classify: a
// notification arrives as exactly the event Message the binary wire would
// have carried, the terminal error envelope arrives as the exception
// Message (ADR-0016's terminal contract downstream applies unchanged),
// and the terminal result envelope is the stream's clean end — it
// surfaces as nullopt, never as a message. A frame the codec refuses is
// Error::Serialization, terminal for the session like any malformed frame.
//
// The opening request envelope never passes through here: generated code
// sends and reads it on the inner socket before constructing the wrapper.
// Everything else — one-receiver discipline, one-outstanding-per-class,
// Close-from-any-thread, backpressure — is the inner socket's contract,
// delegated untouched.
class JsonRpcStreamSocket final : public http::WebSocket {
 public:
  // `id` is the opening call's id, echoed into every notification and
  // matched against every inbound frame.
  JsonRpcStreamSocket(std::shared_ptr<http::WebSocket> inner, Document id);

  // The borrowing form, mirroring EventStream's borrowed constructor: the
  // blocking serve seam owns its socket only for the route callback's
  // lifetime, and the wrapper must not outlive it — the caller keeps
  // `inner` alive past every call on this object.
  JsonRpcStreamSocket(http::WebSocket& inner, Document id);

  Outcome<std::optional<Message>> Receive() override;
  Outcome<Unit> Send(const Message& message) override;
  void Close() override;
  void ReceiveAsync(ReceiveCallback callback) override;
  void SendAsync(const Message& message, SendCallback callback) override;
  bool SupportsAsync() const override;

 private:
  // Engaged only by the owning form; every call goes through inner_.
  std::shared_ptr<http::WebSocket> owner_;
  http::WebSocket* inner_;
  Document id_;
};

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_
