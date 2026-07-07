# Changelog

All notable changes to smithy-cpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow the
policy in [docs/versioning.md](docs/versioning.md).

## [0.1.0] - 2026-07-07

The first tagged release: a vendor-neutral Smithy → C++ code generator, the
runtime it targets, and Bazel-native consumption — with every generated
surface conformance-tested and integration-tested in CI.

### Protocols

- **`alloy#simpleRestJson`** (REST/JSON): the full HTTP binding surface —
  labels (incl. greedy), query, headers, prefix headers, payloads
  (JSON-encoded string payloads per alloy's conformance model),
  `@httpResponseCode`, content negotiation — with neutral `X-Error-Type`
  error identity and status-code fallback. Green against alloy's official
  conformance suite (documented, shrinking exclusion list).
- **`smithy.protocols#rpcv2Cbor`** (RPC/CBOR): green against the official
  Smithy conformance suite, including the recursion and defaults cases.
- **`smithy.cpp.protocols#jsonRpc2`** (RPC/JSON): JSON-RPC 2.0 over a single
  POST endpoint, defined in-repo with an authored conformance suite; interop
  pinned against hand-rolled JSON-RPC peers.

### Generator

- Types (structs, unions over `std::variant`, enums with unknown-value
  preservation, intEnums), Document-pivot serde, clients, servers, generated
  BUILD files, smoke tests, and seeded random integration suites per fixture.
- `@default` population (including `@input` client-skip/server-fill semantics
  and `@required @default` evolution leniency), boxed recursion via
  `smithy::Boxed<T>`, `@required` response headers, `@idempotencyToken`
  auto-fill, `@paginated` paginators, gzip `@requestCompression`,
  constraint validation with suite-exact `ValidationException` output.
- Consumer Bazel rules (`smithy_cpp_{types,client,server}_library`) run the
  generator hermetically inside the build graph; out-of-tree consumer module
  tested in CI on Linux/macOS/Windows.

### Runtime

- `smithy::Document` pivot, JSON (nlohmann-backed) and hand-rolled CBOR
  codecs (RFC 8949 vectors + fuzzers), `Outcome`/`Error` model, retries with
  full-jitter exponential backoff, client interceptors, server middleware,
  W3C trace-context helpers, `@httpBearerAuth`/`@httpApiKeyAuth` wiring.
- Transports: in-memory loopback and dependency-free socket client/server
  for tests and simple deployments; **Boost.Beast production transports both
  directions** — `BeastServerTransport` (thread pool, keep-alive, timeouts,
  size limits, graceful drain, TLS termination) and `BeastHttpClient`
  (keep-alive connection pool, per-request timeouts, TLS via BoringSSL with
  certificate + hostname verification on by default).
- Fuzz harnesses (JSON, CBOR, URI, server dispatch) and a Google Benchmark
  suite (serde, codecs, per-protocol request round trips) run in CI.

[0.1.0]: https://github.com/aaylward/smithy-cpp/releases/tag/v0.1.0
