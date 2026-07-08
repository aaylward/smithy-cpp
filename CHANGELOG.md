# Changelog

All notable changes to smithy-cpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow the
policy in [docs/versioning.md](docs/versioning.md).

## [Unreleased]

The 0.1.0 milestone: a vendor-neutral Smithy → C++ code generator, the runtime
it targets, and Bazel-native consumption — with every generated surface
conformance-tested and integration-tested in CI. Developed on `main` and
**not yet tagged**; the runtime reports `0.1.0-dev` until the first signed
release (see [docs/versioning.md](docs/versioning.md)). Consumers pin a commit
via `git_override` until then.

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
- Fuzz harnesses (JSON, CBOR, URI, server dispatch, regex) and a Google
  Benchmark suite (serde, codecs, per-protocol request round trips, real-TCP
  transport round trips incl. Beast and Beast TLS) run in CI.
- CBOR decoder rejects additional-information 31 on integers and tags
  (RFC 8949 §3.3 not-well-formed encodings previously decoded as 0 / -1 /
  an ignored tag), found by the hostile corpus below.

### Testing & CI (issue #48)

- **Compile-the-output harness** (`codegen/compile-tests/`): the generator
  runs inside the Bazel graph on a hostile gauntlet model — C++ keyword
  member names, quote/backslash/newline enum values, raw-string delimiter
  attacks, int64-extreme bounds/defaults, recursion, keyword union variants —
  and CI compiles the result for every protocol, client and server mode both.
  Issue #43's whole bug class now fails CI instead of a consumer's build.
- Curated hostile CBOR corpus (`cbor_hostile_test.cc`): systematic
  truncations, reserved encodings, indefinite-length abuse, depth bombs,
  boundary integers/halves, and an every-strict-prefix-rejects property, as
  the CBOR counterpart of the vendored JSONTestSuite bank.
- Direct unit tests for `core/uuid.cc` (format, version/variant bits,
  uniqueness, thread-local streams) and `client/observability.cc`
  (attempt observations, trace-context propagation).
- The regex ReDoS bound is a deterministic step-count assertion
  (`Search(text, &steps)` instrumentation) instead of a wall-clock limit.
- `make verify` / `make verify-full`: one-command local verification
  mirroring the CI jobs one-to-one.

[Unreleased]: https://github.com/aaylward/smithy-cpp/commits/main
