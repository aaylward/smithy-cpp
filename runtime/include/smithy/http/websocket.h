#ifndef SMITHY_HTTP_WEBSOCKET_H_
#define SMITHY_HTTP_WEBSOCKET_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "smithy/eventstream/frame.h"
#include "smithy/http/headers.h"
#include "smithy/http/transport.h"

namespace smithy::http {

// A blocking, full-duplex event-stream session over one WebSocket
// connection (ADR-0015). What travels is eventstream::Message — exactly one
// frame per binary WebSocket message; text messages and malformed frames
// fail the session. (On a session negotiated to the JSON-text mode of
// ADR-0018 the wire encoding flips — one JSON envelope per text message,
// binary messages fail — but this facade is unchanged: the transport
// translates, and both modes carry the same Messages.) Both ends of the
// wire speak this type: the server side arrives in
// BeastServerTransport::Options::on_websocket, the client side from
// BeastWebSocketClient::Dial.
//
// Full duplex means one receiving thread and one sending thread may block
// concurrently (mirroring WebSocket's own one-read + one-write model);
// concurrent Send calls are serialized, not rejected, while concurrent
// Receive calls contend for messages in unspecified order (not detected) —
// keep one receiver. Backpressure is
// real on both sides: Send returns when its frame is on the wire, and a
// receiver that stops calling Receive pauses the wire after a small
// internal buffer fills. A paused reader also pauses the keep-alive
// machinery, so come back for your messages within the idle timeout —
// a receiver that never returns is eventually indistinguishable from a
// vanished peer, and the session ends on the idle deadline.
//
// Canonical serve/consume loop:
//
//   while (true) {
//     auto message = socket.Receive();
//     if (!message.ok()) break;              // wire failed: log and stop
//     if (!message->has_value()) break;      // peer closed cleanly: done
//     ... handle **message, socket.Send(reply) ...
//   }
//
class WebSocket {
 public:
  virtual ~WebSocket() = default;

  // Blocks for the next message. nullopt is the peer's clean close — the
  // stream's natural end, not an error, and the reverse of
  // eventstream::DecodeMessage's nullopt: it never means "try again"
  // (another Receive just returns nullopt again, immediately). Errors are
  // permanent for the session: a broken wire, a protocol violation (text
  // message, a binary message that is not exactly one event-stream
  // frame), or Stop()/Close.
  virtual Outcome<std::optional<eventstream::Message>> Receive() = 0;

  // Blocks until the message's frame is on the wire (natural backpressure —
  // nothing queues unboundedly). Fails with Error::Validation for a message
  // the codec refuses to encode (the session is untouched and stays
  // usable), Error::Transport once the session is closed or broken —
  // including a Close() from another thread while this call is blocked.
  virtual Outcome<Unit> Send(const eventstream::Message& message) = 0;

  // Initiates the close handshake; idempotent, non-blocking, and safe
  // from any thread — this is how another thread unblocks a blocked
  // Receive (which then returns nullopt, or an error if the close fails)
  // OR a blocked Send (which fails with a transport error; its frame may
  // be mid-wire, so the close aborts the connection rather than finishing
  // the write, and the peer then observes an error instead of a clean
  // end). The peer's acknowledging close surfaces through Receive the
  // same way. To end a session, Close(); never use destruction as
  // cross-thread cancellation.
  virtual void Close() = 0;
};

// One streaming dial as generated clients describe it (ADR-0016): where to
// connect (host, port, TLS — derived from the client's http(s) endpoint, so
// nothing is configured twice), the upgrade GET's target with its bound
// labels and query, and the headers that ride the upgrade request.
// BeastWebSocketClient::Dialer() consumes it by building Dial's Options;
// custom dialers (ClientConfig::websocket_dialer — how tests run streams
// without Beast) receive it verbatim.
struct WebSocketDialRequest {
  std::string host;
  // 0 means the scheme default: 443 with tls, 80 without.
  int port = 0;
  bool tls = false;
  // Verification knobs when `tls` is true (ClientConfig::tls).
  TlsOptions tls_options;
  // The request target of the upgrade GET (the streaming endpoint).
  std::string target = "/";
  // Extra headers on the upgrade request — bearer tokens, api keys.
  Headers headers;
  // The per-phase budget for the connect, TLS, and upgrade handshakes.
  int handshake_timeout_ms = 30000;
  // After the upgrade: how long a silent connection stays up. Keep-alive
  // pings run underneath, so a healthy-but-quiet stream survives and a
  // vanished peer is detected without any application ping protocol.
  int idle_timeout_seconds = 300;
};

// The dialer a generated streaming client calls: one WebSocketDialRequest
// in, one connected session out (ADR-0016). ClientConfig::websocket_dialer
// carries an injected one; BeastWebSocketClient::Dialer() is the default.
using WebSocketDialer =
    std::function<Outcome<std::shared_ptr<WebSocket>>(const WebSocketDialRequest&)>;

// Dials a WebSocket connection carrying event-stream messages (ADR-0015):
// resolve, connect, TLS (ADR-0007 posture: TLS 1.2 floor, certificate and
// hostname verification on by default, SNI), then the WebSocket upgrade
// handshake — each phase under the handshake_timeout_ms budget (name
// resolution uses the system resolver's own timeout). The returned
// session owns one background io thread for the connection's lifetime.
// Give each thread its own copy of the returned shared_ptr and destroy
// the handle only after every Send/Receive has returned — Close() is the
// cancellation path, destruction is not.
//
//   auto socket = smithy::http::BeastWebSocketClient::Dial({
//       .host = "stream.example.com", .port = 443, .tls = true,
//       .target = "/events",
//   });
//   if (!socket.ok()) { ... }
//   (*socket)->Send(...); (*socket)->Receive();
class BeastWebSocketClient {
 public:
  struct Options {
    std::string host;
    // 0 means the scheme default: 443 with tls, 80 without.
    int port = 0;
    bool tls = false;
    // Verification knobs when `tls` is true — the same struct ClientConfig
    // carries (beast_transport.h precedent), so wiring cannot drift.
    TlsOptions tls_options;
    // The request target of the upgrade GET (the streaming endpoint).
    std::string target = "/";
    // Extra headers on the upgrade request — bearer tokens, api keys: the
    // server's websocket_gate sees these before any upgrade completes.
    Headers headers;
    // Offer the negotiated JSON-text frame mode (ADR-0018) on the dial:
    // an echoed subprotocol selects text framing, no echo falls back to
    // the binary wire silently — both modes carry the same messages, so
    // the difference is invisible above the session. For parity, tooling,
    // and the negotiation tests; browsers are the JSON wire's audience,
    // and native clients should keep the default. A server that answers
    // with a subprotocol never offered fails the dial.
    bool offer_json_frames = false;
    // The per-phase budget for the connect, TLS, and upgrade handshakes.
    int handshake_timeout_ms = 30000;
    // After the upgrade: how long a silent connection stays up. Keep-alive
    // pings run underneath, so a healthy-but-quiet stream survives and a
    // vanished peer is detected without any application ping protocol.
    int idle_timeout_seconds = 300;
  };

  static Outcome<std::shared_ptr<WebSocket>> Dial(Options options);

  // Dial in WebSocketDialer form — what a generated streaming client uses
  // when ClientConfig::websocket_dialer is unset (ADR-0016). Declared here,
  // implemented in the Beast TU. The link reality: a generated streaming
  // client names this fallback unconditionally, so its binaries ALWAYS
  // link Boost, injected dialer or not — only hand-written wiring that
  // injects a dialer and never mentions this class stays dep-light.
  static WebSocketDialer Dialer();
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_WEBSOCKET_H_
