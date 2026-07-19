# ADR-0013: Transport connection failures are observable events, silence means healthy

**Status:** Accepted (2026-07-19)

## Context

The last open #46 item: `Observe` middleware sees only requests that reach
the handler chain. Framing errors, per-read timeouts, TLS handshake
failures, and dropped connections are all handled inside
`BeastServerTransport` and end in a silent close — metrics undercount
exactly the failures that matter during an incident (a load balancer
misrouting TLS to the plain port, a slowloris stall, a client fleet
resetting mid-response). #102's `Options::on_rejected` proved the pattern
for the 413/431 rejections: a contained transport-level observer, wired by
the consumer to the same sink as `Observe`. This ADR extends that pattern
to the remaining failure classes, and settles what deliberately stays
silent.

The #91 relationship runs signals-before-sinks: the Prometheus middleware
will consume these events, but the events do not need it — exactly as
`on_rejected` shipped standalone.

## Decision

`BeastServerTransport::Options` gains `on_connection_event`, a contained
observer sibling of `on_rejected`, delivering `ConnectionEvent{kind,
peer_address, detail, elapsed}`:

- **`kTlsHandshakeFailure`** — TLS was configured and a handshake went
  *wrong*: plaintext to the TLS port, a version/cipher/ALPN mismatch. The
  TCP peer is known before TLS, so the event carries it. Handshakes that
  never really began — the peer connected and left (eof/`stream_truncated`)
  or idled into the deadline — are the TCP-health-probe and port-scanner
  shape and stay silent: that noise scales with infrastructure, not
  incidents.
- **`kFramingError`** — bytes arrived that never parsed into a request
  (the parse-error family of Beast's HTTP error category).
- **`kReadTimeout`** — a request that had *begun* (at least one octet of
  the message parsed) stalled past `request_timeout_seconds`: the
  slowloris shape.
- **`kDropped`** — the peer vanished mid-request (`partial_message`, or a
  transport-level error like a reset while reading) or mid-response (a
  write error).

`detail` is the transport's own error text (`error_code::message()`);
`elapsed` is time spent in the failing phase (handshake start, read start,
or write start — a `steady_clock` diff). The peer is captured when the
phase *begins*, because by failure time the socket may no longer report it
(a timed-out stream is closed by the deadline machinery before its handler
runs; an RST during handler execution empties `getpeername` before the
write starts). The handshake and read phases look it up at phase start
only when the hook is installed, so the unobserved path pays nothing; the
write phase reuses the request's own `peer_address` stamp. The observer
runs on the io thread, is contained like `on_rejected` and the middleware
hooks (a throwing sink logs and never takes down the connection path it
is watching), and must be cheap and thread-safe.

**Deliberately silent — silence means healthy:**

- A clean close at a message boundary: `end_of_stream`, and its TLS twin
  `stream_truncated` — a peer that skips close_notify, which this
  runtime's own client and `CloseStream` do themselves, tolerated HTTP
  practice. (Mid-message, both shapes report `kDropped`.)
- A timeout with *nothing* received: indistinguishable from healthy idle
  keep-alive reaping, and emitting it would swamp the signal in proportion
  to healthy traffic. The zero-byte slowloris variant is bounded by
  `max_connections`, not observability.
- Probe-shaped handshake non-starts (connect-and-leave, idle-out), per the
  `kTlsHandshakeFailure` definition above.
- Everything during `Stop()`: shutdown cancellations are lifecycle, not
  incident.
- The over-limit write path: that connection was already observed once via
  `on_rejected`; a second event would double-count it.

Scope: `BeastServerTransport` only — `SocketHttpServer` is test-only
(ADR-0006 relegation), and the client side reports failures as `Outcome`
errors already. Accept-loop errors (fd exhaustion) are a separate,
connection-less signal and stay out; if a consumer needs them, that is a
new kind, not a reinterpretation of these.

## Consequences

- The #46 observability tail closes: every transport-terminated connection
  is either observable or deliberately, documented-ly healthy.
- Each wire phase now gets its own `request_timeout_seconds` budget: Beast
  expiries are absolute and outlive the op, so `Respond` re-arms the
  deadline before writing. Without that, the write ran under the deadline
  set at read start, and a handler outrunning the residue had its response
  cancelled — which this ADR's events would have misreported as a peer
  drop. Handler time is bounded by the drain/executor policy, not the wire
  deadline.
- `Observe`'s duration remains handler-chain time by design; wire-phase
  time arrives on the events themselves (`elapsed`), so #91's metrics
  middleware can compose full-request pictures without `Observe` growing a
  transport dependency.
- The production guide wires `on_connection_event` to the same sink as
  `on_rejected` and `Observe`. The misrouting alarm reads both ways: a
  `kTlsHandshakeFailure` flood at the TLS port is plaintext arriving
  there, a `kFramingError` flood at the plain port is TLS ClientHellos
  arriving *there*; `kReadTimeout` is the stall alarm.
- Event kinds are a closed enum; new failure classes are new kinds
  (additive), never reinterpretations of existing ones.
