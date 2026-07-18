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
  tested in CI on Linux/macOS.
- Generated handler methods take the request metadata alongside the typed
  input: `Op(const OpInput&, const smithy::server::RequestContext&)`, where
  the context carries the raw request (unmodeled headers, the inbound
  `traceparent`, the transport-stamped peer address) plus the decoded routing
  captures (issue #46). **Breaking** for commit-pinning consumers: existing
  handler implementations add the parameter (unnamed when unused).

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
- Server middleware additions for production serving: `Guard` admission
  control (rate limiting, allowlists, maintenance mode — policy stays an
  application dependency) with a `TooManyRequests` reject factory,
  `HealthEndpoint` static liveness, and an optional `Observe` `on_start`
  callback for in-flight gauges with guaranteed start/complete pairing.
  **Breaking:** `Observe(callback, now)` call sites become
  `Observe(callback, nullptr, now)`.
- Fuzz harnesses (JSON, CBOR, URI, server dispatch, regex) and a Google
  Benchmark suite (serde, codecs, per-protocol request round trips, real-TCP
  transport round trips incl. Beast and Beast TLS) run in CI.
- CBOR decoder rejects additional-information 31 on integers and tags
  (RFC 8949 §3.3 not-well-formed encodings previously decoded as 0 / -1 /
  an ignored tag), found by the hostile corpus below.

### Removed

- **Windows support** (ADR-0008, issue #58). Linux and macOS are the
  supported platforms, both sanitizer-covered in CI. The socket transport is
  POSIX-only (no winsock/WSAStartup fabric), the Bazel rules carry no
  Windows `select()`s or `build:windows` config, and the Windows CI jobs are
  gone. Re-adding Windows would be a port driven by a concrete consumer, not
  a revert.

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
- **HTTP/1.1 parser extracted and fuzzed** (`smithy/http/http1.h`): the
  socket transports' hand-rolled message reader is now a pure,
  callback-fed function with a libFuzzer harness (`//fuzz:http1_fuzz`, in
  the CI smoke loop) and a platform-independent hostile bank
  (`http1_hostile_test.cc`) covering smuggling framing, hostile
  content-lengths, truncation-everywhere, and header floods. Hardening
  found while banking: an empty or `+`-signed Content-Length previously
  parsed as a valid length; both now reject (digits-only per RFC 9110).
- **Malformed-server coverage evened out**: hand-written suites pin how the
  generated simpleRestJson and rpcv2Cbor servers reject hostile requests
  (unparseable bodies, protocol-precondition violations, wrong
  content-type/method/route) the way jsonrpc2's generated suite always did —
  including the previously-unasserted simpleRestJson `@pattern`-violation
  wire message.
- **Union x protocol conformance cells filled** (`protocol-tests/unions/`):
  the cbor and jsonRpc2 union cells — previously reliant on coin-flip random
  integration tests that only prove serde self-consistency — now pin the
  wire subdocument for every union variant deterministically in all four
  directions (client encode/decode, server decode/echo), plus the reject
  cells (empty, multi-member, unknown-member, null-member) and the `__type`
  discriminator tolerance.
- **Union member-type gauntlet** (`protocol-tests/unions/` + issue #56):
  a generated-in-graph UnionGauntlet service extends the union cells to
  blob, timestamp, list, map, enum, intEnum, and recursive-struct members,
  each pinned in four directions per protocol, with a hand-derived
  byte-exact request vector (RFC 8949 deterministic encoding / compact
  sorted JSON) and the error-shape cell: a modeled error whose union member
  rides next to its `__type` discriminator, both wire-inspected and
  round-tripped into the typed error detail. The existing reject cells now
  pin their diagnoses (exactly-one-member, unknown-member, non-map), not
  just the rejection; the `__type` tolerance is documented in
  docs/generated-types.md as the serde contract.
- **Sanitizers run on macOS too**: the asan+ubsan CI job is now a
  linux/clang + macos/apple-clang matrix, covering the transport layer's
  Apple-specific paths (SO_NOSIGPIPE, libc++) — every supported platform is
  now sanitizer-covered.
- **Golden self-ratification closed** (`GoldenProtocolTestAuditTest`): the
  byte-identical regeneration check validates the goldens against the same
  generator that produced them, so a generator bug that dropped or rewrote
  conformance vectors could ratify itself. A new audit enumerates the test
  cases from the upstream suite definitions (the alloy and Smithy
  conformance jars, and the authored jsonRpc2 model) using only the
  upstream smithy-model API and asserts every case is either in the
  committed golden test sources or in the must-shrink exclusion list — plus
  per-case wire facts (method/status), no phantom tests, and no exclusion
  naming a nonexistent upstream case. Together with the generator's
  stale-exclusion guard, the seam is now watched from both sides.
- **Generator-class unit tests**: direct Java suites for the previously
  untested generator internals — CppLiterals (the issue-#43 escaping
  chokepoint: octal escapes, int64-min idiom, float literal typing),
  CppReservedWords (keyword vs macro boundary), ProtocolSupport bounds,
  CppSettings validation, MemberDefaults (the @default/@input/@required
  semantics matrix), RecursionIndex (boxing decisions and refused cycles),
  plus emitted-source suites for SerdeGenerator (required-member errors,
  dense-null rejection, union exactly-one arithmetic, @timestampFormat),
  ValidationGenerator (suite-exact messages, compilable bounds, one-time
  pattern compilation, code-point vs element length), and
  BuildFileGenerator (target set per mode, runtime target wiring,
  emitBuildFile). 43 new tests; generator unit coverage was 2 of 31
  classes before this.
- **Code-coverage tooling**: a `coverage` CI job runs
  `bazel coverage --combined_report=lcov` over the runtime, prints the
  per-module summary, and uploads the rendered HTML report as an artifact;
  `make coverage` runs the same locally. Measurement only — no gate yet.

[Unreleased]: https://github.com/aaylward/smithy-cpp/commits/main
