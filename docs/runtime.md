# smithy-cpp runtime overview

The runtime (`//runtime`, headers under `smithy/...`) is the hand-written
library that generated clients and servers link against. Generated code stays
thin glue; behavior lives here (PLAN §2). Modules mirror smithy-rs's runtime
crates (PLAN §3.2a).

| Bazel target | Namespace | Contents |
|---|---|---|
| `//runtime:core` | `smithy` | `Outcome<T, E>` + `Error` (ADR-0003), `Blob`, `Timestamp` (epoch-seconds / RFC 3339 date-time / IMF-fixdate http-date), `Document` (dynamic value + serde pivot), base64 |
| `//runtime:json` | `smithy::json` | `Document` ⇄ JSON text via nlohmann (blobs as base64, timestamps per stored format) |
| `//runtime:cbor` | `smithy::cbor` | `Document` ⇄ deterministic CBOR (RFC 8949; tag-1 timestamps; tolerant decoder) — ADR-0005 |
| `//runtime:http` | `smithy::http` | `Headers` (case-insensitive), URI percent-encoding per the Smithy HTTP binding rules, `HttpRequest`/`HttpResponse`, `HttpClient`/`HttpServerTransport` interfaces, `Loopback` in-memory transport, built-in `SocketHttpClient`/`SocketHttpServer` (test/reference only — ADR-0006) |
| `//runtime:http_beast` | `smithy::http` | `BeastServerTransport` (ADR-0006): the production server transport on BCR modular Boost.Beast/asio — concurrent connections on a thread pool, keep-alive, per-connection timeouts, body- and header-size limits, graceful drain on Stop. Separate target so Boost stays out of dep-light builds |
| `//runtime:client` | `smithy` | `ClientConfig` (endpoint, timeout, user-agent, transport injection, `RetryPolicy`, request-compression threshold, `Interceptor` hooks around every attempt), `SendWithRetries` (full-jitter exponential backoff over transport errors and 429/5xx — see docs/production-guide.md) |
| `//runtime:compression` | `smithy` | `GzipCompress`/`GzipDecompress` (zlib; decompression-bomb guard, trailing-garbage rejection) backing `@requestCompression` |
| `//runtime:server` | `smithy::server` | `Router` (literal > label > greedy precedence, 404/405/400), `RequestContext`, `MakeErrorResponse`, `ValidationFailure`, user-supplied `Middleware` + `Chain` + the `Observe` logging/metrics hook |

## Design rules

- **Errors are values.** Everything that can fail returns
  `Outcome<T>`; exceptions never cross public boundaries (ADR-0003). Modeled
  service errors carry `ErrorKind::kModeled` and the error shape name in
  `Error::code()`.
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
