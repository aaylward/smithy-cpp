# smithy-cpp runtime overview

The runtime (`//runtime`, headers under `smithy/...`) is the hand-written
library that generated clients and servers link against. Generated code stays
thin glue; behavior lives here (PLAN §2). Modules mirror smithy-rs's runtime
crates (PLAN §3.2a).

| Bazel target | Namespace | Contents |
|---|---|---|
| `//runtime:core` | `smithy` | `Outcome<T, E>` + `Error` (ADR-0003), `Blob`, `Timestamp` (epoch-seconds / RFC 3339 date-time / IMF-fixdate http-date), `Document` (dynamic value + serde pivot), base64, `Regex` (linear-time NFA engine behind generated `@pattern` validation — ReDoS-safe) |
| `//runtime:json` | `smithy::json` | `Document` ⇄ JSON text via nlohmann (blobs as base64, timestamps per stored format) |
| `//runtime:cbor` | `smithy::cbor` | `Document` ⇄ deterministic CBOR (RFC 8949; tag-1 timestamps; tolerant decoder) — ADR-0005 |
| `//runtime:eventstream` | `smithy::eventstream` | Event-stream message framing (ADR-0014): CRC-guarded prelude, ten typed header wire types, opaque `Blob` payload; strict fail-closed incremental decoder with symmetric encode/decode bounds. Plus the generated streaming API's substrate (ADR-0016): the envelope helpers (`:message-type`/`:event-type`/`:exception-type` — the one place the header convention lives) and `EventStream<Tx, Rx>`, the typed blocking session generated streaming operations return and handlers receive |
| `//runtime:eventstream_json` | `smithy::eventstream` | The negotiated JSON-text wire encoding (ADR-0018): `EncodeJsonFrame`/`DecodeJsonFrame` between envelope-bearing `Message`s and the `{"event": ..., "payload": ...}` text frames a browser speaks with `JSON.parse`, plus `kJsonFramesSubprotocol` (`smithy.eventstream.v1+json`). Separate target so `:eventstream` stays a leaf on `:core` |
| `//runtime:http` | `smithy::http` | `Headers` (case-insensitive), URI percent-encoding per the Smithy HTTP binding rules, `HttpRequest`/`HttpResponse`, `HttpClient`/`HttpServerTransport` interfaces, `Loopback` in-memory transport, built-in `SocketHttpClient`/`SocketHttpServer` (test/reference only — ADR-0006), W3C `TraceContext` parse/format/generate, the transport-neutral `WebSocket` interface + `InMemoryWebSocketPair` (the streaming Loopback analog, so generated streaming tests run without Boost — ADR-0016) |
| `//runtime:http_beast` | `smithy::http` | `BeastServerTransport` (ADR-0006): the production server transport on BCR modular Boost.Beast/asio — concurrent connections on a thread pool, keep-alive, per-connection timeouts, body- and header-size limits, graceful drain on Stop. WebSocket upgrades ride the same transport (ADR-0015): `Options::on_websocket`/`websocket_gate` serve blocking full-duplex `smithy::http::WebSocket` sessions carrying event-stream messages, and `BeastWebSocketClient::Dial` is the client end. `Options::websocket_accept_json_frames` negotiates the ADR-0018 JSON-text wire for clients offering the subprotocol (browsers), transparently to everything above the session. Separate target so Boost stays out of dep-light builds |
| `//runtime:client` | `smithy` | `ClientConfig` (endpoint, timeout, user-agent, transport injection, `RetryPolicy`, request-compression threshold, `Interceptor` hooks around every attempt, `bearer_token`/`api_key` credential providers), `SendWithRetries` (full-jitter exponential backoff over transport errors and 429/5xx), `ObserveAttempts` + `PropagateTraceContext` interceptors — see docs/production-guide.md |
| `//runtime:compression` | `smithy` | `GzipCompress`/`GzipDecompress` (zlib; decompression-bomb guard, trailing-garbage rejection) backing `@requestCompression` |
| `//runtime:server` | `smithy::server` | `Router` (literal > label > greedy precedence, 404/405/400), `RequestContext`, `MakeErrorResponse`, `ValidationFailure`, user-supplied `Middleware` + `Chain`, the `Observe` logging/metrics hook, `RequireBearerAuth`/`RequireApiKeyHeader` guards, `WebSocketRouter` — the streaming routes' method + URI-pattern dispatch behind generated `StreamRouter()`, sharing `Router`'s matching by construction (ADR-0016) — `RequireOrigin`, the browser-facing Origin-allowlist `websocket_gate` (ADR-0018) — and `SessionRegistry` — the multi-client fan-out helper over owning `EventStream::Share()` handles: bounded per-session outbound queues, non-blocking `SendTo`/`Broadcast` with per-recipient construction, a slow-consumer policy, and `Drain` for graceful shutdown (ADR-0017) |

## Design rules

- **Errors are values.** Everything that can fail returns
  `Outcome<T>`; exceptions never cross public boundaries (ADR-0003). Modeled
  service errors carry `ErrorKind::kModeled` and the error shape name in
  `Error::code()`. Dereferencing the side an `Outcome` doesn't hold — or a
  generated union's wrong case — is a contract violation that terminates the
  process with context (ADR-0009); `value_or_die("context")` adds caller
  context to that crash line. Note the server consequence: a handler's
  wrong-side deref used to surface as a contained 500, and is now fatal —
  a contract bug is a bug, not a request-scoped failure. (`Document`'s
  `as_*` accessors are outside this rule for now; see ADR-0009.)
- **`Document` is the serde pivot** (ADR-0005): typed structs ⇄ `Document` ⇄
  bytes. Blob and timestamp nodes let each codec apply its own wire rules.
- **Transports are interfaces.** Generated code only sees
  `HttpClient`/`HttpServerTransport`. `Loopback` wires a client directly to a
  server handler for fast in-process integration tests; the socket transport
  runs the same test bodies over real TCP. Richer transports (TLS, pooling,
  HTTP/2) are future adapters, not API changes.
- **Streaming-readiness:** `http::Body` is an alias precisely so Phase 8 can
  grow it into a stream-shaped type without rewriting signatures.

## Worked example

`examples/weather/handwritten/` is a complete hand-written client and server
for `examples/weather/model/weather.smithy`, built only on the runtime — the
prototype the Phase 2/3/4 generators must reproduce. Its
`weather_e2e_test` runs every operation (including a modeled 404 and
percent-encoded labels) through the same assertions over both `Loopback` and
real sockets:

```sh
bazel test //examples/weather:weather_e2e_test
```
