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
  size limits with the 413/431 rejections observable via
  `Options::on_rejected`, graceful drain, TLS termination) and `BeastHttpClient`
  (keep-alive connection pool, per-request timeouts, TLS via BoringSSL with
  certificate + hostname verification on by default).
- Connections the transport terminates without a response are observable
  (ADR-0013): `BeastServerTransport::Options::on_connection_event` reports
  TLS handshake failures (handshakes that went wrong, not probe non-starts),
  framing garbage, stalled reads (the slowloris shape), and mid-stream
  drops, each with the peer, the error text, and phase-elapsed time — while
  clean closes (with or without TLS close_notify), idle reaping, and
  shutdown stay deliberately silent. Each wire phase now gets its own
  `request_timeout_seconds` budget, so a handler outrunning the read
  deadline's residue no longer has its response cancelled.
- Server trace identity minted at transport ingress (ADR-0011): a valid
  inbound `traceparent` continues verbatim; an absent or malformed one is
  replaced with a fresh root context, so `Observe`'s `trace_parent` always
  parses and any 5xx leaving the handler chain — returned or thrown —
  carries the request's trace id as `x-correlation-id` (a handler-set id
  wins).
- Proxy-aware client identity (ADR-0012): `smithy::http::ClientAddress`
  derives the real client behind a reverse proxy from `peer_address` and
  `x-forwarded-for` — anchored at the L4 peer, walking rightmost-untrusted
  against a `TrustedProxies` CIDR set, so a spoofed entry from outside the
  trust boundary never wins; the no-proxy topology is the explicit
  `TrustedProxies::None()`, never a default constructor (issue #104).
  `smithy::server::PerClientRateLimit` ships the derivation-into-admission
  wiring (the pluggable `allow(client)` policy sees only the derived key;
  underivable requests are admitted), and `DeriveClient` reports each
  address's derivation `Source` so a drifted trust boundary shows up on a
  dashboard instead of as a silent one-bucket collapse. The production
  guide teaches the composed middleware plus the `TRUSTED_PROXY_CIDRS`
  plumbing convention.
- Server middleware additions for production serving: `Guard` admission
  control (allowlists, maintenance mode — policy stays an application
  dependency) with a `TooManyRequests` reject factory,
  `HealthEndpoint` static liveness, and an optional `Observe` `on_start`
  callback for in-flight gauges with guaranteed start/complete pairing.
  **Breaking:** `Observe(callback, now)` call sites become
  `Observe(callback, nullptr, now)`.
- The browser wire for event streams (ADR-0018, issue #113): a client
  that offers the `smithy.eventstream.v1+json` WebSocket subprotocol on a
  server with `Options::websocket_accept_json_frames` set gets text
  frames carrying a JSON envelope — `{"event": "<member>",
  "payload": {...}}`, `"exception"` for the error arm — so a page speaks
  a generated simpleRestJson stream with `JSON.parse` alone: no codec, no
  build step. Native clients keep the binary wire (no offer, headerless
  101, byte-identical to before); the translation is transport-internal
  (`//runtime:eventstream_json`, `EncodeJsonFrame`/`DecodeJsonFrame`), so
  `EventStream`, the routers, `SessionRegistry`, and generated code are
  untouched; and the fail-closed posture transposes — binary frames and
  unknown envelope members fail a JSON session the way text fails a
  binary one. `BeastWebSocketClient::Options::offer_json_frames` offers
  the mode client-side for parity and tests (silent binary fallback when
  not accepted; a server selecting an unoffered subprotocol fails the
  dial). `smithy::server::RequireOrigin({...})` is the composable
  Origin-allowlist gate browser-facing endpoints need (scheme + host +
  port exact; absent Origin admitted — hijacking defense, not auth), and
  the production guide now names the blessed browser auth pattern
  (short-lived single-use tickets in an `@httpQuery` member, validated by
  the gate before the 101) with its caveats said out loud.
- Completion-driven event streams (ADR-0019) — the async adapter ADR-0014
  through ADR-0017 name as future work, runtime slice: `WebSocket` grows
  `ReceiveAsync`/`SendAsync`/`SupportsAsync` (one outstanding per class,
  completions on the transport's completion context; native on the Beast
  sessions and the in-memory pair, polite refusals by default so custom
  sockets keep working). `BeastServerTransport::Options::on_websocket_session`
  is the shared-session sibling of `on_websocket`: the callback owns the
  session and returns immediately, so a stream no longer parks a
  handler-pool thread — `handler_threads` returns to sizing unary work.
  `smithy::eventstream::AsyncEventStream<Tx, Rx>` + `Detached` put
  `co_await` where the blocking facade parks a thread (same terminal
  semantics as `EventStream`, same `Share()` handles), and
  `SessionRegistry Options::async_delivery` drains each session's queue
  through `EventStreamHandle::SendAsync` completion chains instead of
  writer threads (per-session fallback for blocking-only sockets). The
  thread-free chat hub (`examples/chat/async_hub_*`) serves the same
  generated wire through the new seam, driven as real shell-commanded
  processes. Generated handler/client surfaces stay blocking — the
  coroutine lift for generated code is the follow-up ADR-0019 gates.
- Streaming routes on the shared seam (issue #118): `WebSocketRouter` grows
  `AddSession`/`ServeSession()`, the `on_websocket_session` parallel of
  `Add`/`Serve()` — same pattern grammar, precedence, conflict rules, and
  seam-agnostic `Gate()`, with the winning route receiving the session as
  an owner (a launch point, not a serve loop). One router serves one seam:
  the transport mounts at most one dispatcher, so `Add` and `AddSession`
  refuse to mix rather than deaden routes silently. The thread-free chat
  hub now mounts its Converse route through the router instead of a
  hand-rolled target parser.
- Event-stream session handles and fan-out (ADR-0017, issue #112):
  `stream.Share()` mints an `EventStreamHandle<Out>` — an owning cheap-copy
  value handle safe to hold beyond the handler's borrow (copies are how a
  session fans out), sending and closing from any thread while the session
  lives and failing softly with `Error::Transport` (never a dangle) once
  it is gone.
  `smithy::server::SessionRegistry<Out>` builds the multi-client hub on
  top: a thread-safe map of handles with a bounded outbound queue and
  writer thread per session, so `SendTo`/`Broadcast` never block on a slow
  client's wire; per-recipient event construction
  (`Broadcast(ids, make)`) for per-viewer redaction; a slow-consumer
  policy (disconnect by default, `Options::on_slow_consumer` to override);
  and `Drain(grace)` — close every session and wait for handlers to unwind
  — as the graceful step before the transport's abort-flavored `Stop()`.
  The consumer reference is the chat hub (`examples/chat/`): rooms,
  redaction, watchers and talkers on one registry, SIGTERM → drain → clean
  exit, tested in memory and as real processes driven by shell commands.
  **Breaking:** `EventStream` is now move-only (copying was never
  meaningful; handles are how a session fans out).
- Phase 8 slice 3, generated event streams (ADR-0016): `@streaming` union
  operations generate real streaming code for `simpleRestJson` and
  `rpcv2Cbor` (`jsonRpc2` refuses with a diagnostic). Clients gain
  `Outcome<EventStream<In, Out>> Op(input)` — the upgrade GET resolves
  `@http` bindings exactly like a unary request, dialing derives from the
  one `ClientConfig` endpoint (an `https` endpoint dials `wss`), and
  `ClientConfig::websocket_dialer` injects a custom dialer the way
  `http_client` injects the unary transport (with
  `smithy::http::InMemoryWebSocketPair`, that is how streams test without
  Boost). Handlers grow
  `Outcome<Unit> Op(input, EventStream<Out, In>&, context)`; generated
  servers expose `StreamRouter()`, a `smithy::server::WebSocketRouter`
  sharing the unary `Router`'s matching, mounted on the transport via
  `websocket_gate`/`on_websocket` in two lines. One event travels per
  binary WebSocket message in the slice-1 framing with an authored,
  vendor-neutral envelope (`:message-type`/`:event-type`/
  `:exception-type`); a handler error becomes one typed exception message
  then a close, surfacing client-side exactly like a unary modeled error
  (kind, code, typed detail). Scope edges are generation-time diagnostics
  (`@eventHeader`/`@eventPayload`, body-bound initial-request members,
  initial-response members), generated smoke/integration suites skip
  streaming operations, and BUILD deps grow only for streaming services.
  PLAN §Phase 8's exit criterion lands as `examples/chat/`: the generated
  chat client and server run full duplex over real WebSockets (TLS
  included) in CI, with an in-memory twin and an out-of-tree consumer
  acceptance test beside it.
- Phase 8 slice 2, WebSocket transports (ADR-0015): `BeastServerTransport`
  upgrades WebSocket requests in place — `Options::websocket_gate` refuses
  with a plain HTTP answer before any 101 exists (auth sees the whole
  request), `Options::on_websocket` serves the accepted session on the
  handler pool — and `BeastWebSocketClient::Dial` is the client end
  (ADR-0007 TLS posture, SNI, hostname verification, one handshake
  budget). Both ends speak `smithy::http::WebSocket`: blocking full-duplex
  `Send`/`Receive`/`Close` carrying one event-stream frame per binary
  WebSocket message, with real backpressure both directions, keep-alive
  pings under an idle timeout, and protocol violations (text messages,
  non-frame payloads) failing the session. Failed upgrades surface as the
  new `ConnectionEvent::Kind::kUpgradeFailure`; `Stop()` aborts live
  sessions so blocked serve callbacks wake. Usable directly ahead of the
  generated streaming API (slice 3).
- Phase 8 groundwork, wire-format-first (ADR-0014): `//runtime:eventstream`
  is the event-stream message framing both streaming protocols are defined
  against — CRC-guarded prelude, ten typed header wire types (headers
  build from plain values and the timestamp is the runtime's own
  `smithy::Timestamp`), opaque `Blob` payloads, an incremental strict
  fail-closed decoder, and symmetric bounds (Encode refuses whatever
  Decode would reject). `Message::FindHeader`/`FindString` cover the
  dispatch-on-`:event-type` lookup every consumer does, and messages
  debug-render. Fuzzed, and pinned with byte-exact vectors from an
  independent implementation.
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
