# smithy-cpp — Phased Implementation Plan

A plan for building **smithy-cpp**: a [Smithy](https://smithy.io) code generator that produces
idiomatic, well-tested C++ **clients** and **servers**, plus the shared C++ **runtime library**
the generated code depends on. A core requirement threaded through every phase: **generated
clients are used to integration-test generated servers**, so the two halves continuously verify
each other.

---

## 1. Goals

1. **Client generation** — from a Smithy model, generate a C++ client SDK: typed request/response
   structures, operation methods, serialization, error handling, and a pluggable HTTP transport.
2. **Server generation** — from the same model, generate a C++ server scaffold: request routing,
   deserialization, input validation, a handler interface the user implements, and response
   serialization.
3. **Round-trip verification** — every example/test service is exercised end-to-end by running the
   generated client against the generated server, in CI, on every commit.
4. **Well tested** — a layered test strategy: runtime unit tests, codegen snapshot ("golden") tests,
   compile tests of generated code, Smithy protocol compliance tests, client↔server integration
   tests, fuzzing and sanitizers.
5. **Documented** — user-facing quick start, per-feature guides, generated-code reference, and
   architecture/decision records for contributors.
6. **Easy to use** — one build-system rule invocation (or one CLI command) to go from
   `model.smithy` to a compiled client or server target, with **first-class support for both
   CMake and Bazel**: Bazel users get native rules (`smithy_cpp_client_library` /
   `smithy_cpp_server_library`), a bzlmod module, and Bazel Central Registry distribution — not a
   wrapper afterthought.

### Non-goals (for the initial releases)

- AWS service SDKs / SigV4 / AWS-specific traits (the design must not preclude them, but they are
  out of scope; the AWS SDK for C++ already exists).
- Every Smithy protocol. We start with **restJson1** (see §3.3) and add others later.
- Event streams, HTTP/2-specific features, and MQTT bindings — deferred (§9).

---

## 2. Guiding principles

- **Thin generated code, rich runtime.** Generated code should be boring glue over a hand-written,
  well-tested runtime library. Bugs get fixed once in the runtime, not N times in templates.
- **Idiomatic C++20.** `std::optional` for optional members, `std::variant`-backed unions,
  `std::expected`-style outcomes (polyfilled until C++23 is table stakes), RAII everywhere, no
  exceptions across the generated API boundary by default (configurable).
- **Pluggable transports.** Generated code targets abstract `HttpClient` / `HttpServer` interfaces;
  concrete implementations (libcurl client, Boost.Beast/asio server) live behind them and are
  swappable — which is also what makes in-process integration testing cheap.
- **Golden tests for codegen.** Every generator feature lands with checked-in expected output; a
  diff in generated code is always a deliberate, reviewed event.
- **The client tests the server.** From the first phase where both exist, CI compiles an example
  service both ways and runs the generated client against the generated server.
- **Stand on smithy-rs's shoulders.** smithy-rs (§3.2a) already solved client+server generation
  for a systems language with the same codegen framework; we port its architecture and test
  strategy to C++ instead of rediscovering them.
- **CMake and Bazel are co-equal.** Every user-facing capability — generation rules, runtime
  consumption, the integration-test pattern — ships and is CI-tested in both build systems.

---

## 3. Architecture

### 3.1 Components

```
smithy-cpp/
├── codegen/                  # The generator (JVM, Smithy DirectedCodegen) — §3.2
│   ├── smithy-cpp-codegen/           # core: symbol provider, type/serde generation
│   ├── smithy-cpp-codegen-client/    # client-specific generation
│   ├── smithy-cpp-codegen-server/    # server-specific generation
│   └── smithy-cpp-codegen-tests/     # golden tests, protocol-test generation
├── runtime/                  # C++ runtime the generated code links against — Phase 1
│   ├── include/smithy/core/          # types: Blob, Timestamp, Document, Outcome, Error
│   ├── include/smithy/json/          # JSON serde primitives
│   ├── include/smithy/http/          # HttpRequest/Response, HttpClient/HttpServer interfaces
│   ├── include/smithy/client/        # client base: config, endpoint, retry, interceptors
│   ├── include/smithy/server/        # server base: router, handler dispatch, validation
│   └── src/…                         # implementations: curl transport, beast server, …
├── cmake/                    # SmithyCpp.cmake: smithy_cpp_generate() integration — Phase 6
├── bazel/                    # Bazel rules: smithy_cpp_*_library, toolchains — Phase 6
├── MODULE.bazel              # bzlmod module; runtime carries BUILD files from Phase 1
├── examples/                 # example models + services (also used as test fixtures)
├── tests/                    # cross-cutting: integration harness, fuzzers
└── docs/                     # user guides, design docs, ADRs
```

### 3.2 Generator technology (decision)

**Decision: write the generator on the JVM (Java 17) using Smithy's official
`software.amazon.smithy:smithy-codegen-core` `DirectedCodegen` framework**, packaged as a
`smithy-build` plugin (like smithy-typescript, smithy-go, smithy-swift, smithy-rs).

Alternative considered: a standalone generator (in C++ or Python) that consumes the JSON AST
produced by `smithy build`. It keeps the toolchain single-language but forfeits the pieces the
Smithy team maintains for us — model validation, trait indexes (`HttpBindingIndex`,
`OperationIndex`), symbol/dependency management, topological shape ordering, protocol-test trait
plumbing, and forward compatibility as the IDL evolves. Every mature third-party Smithy generator
uses the JVM framework; we should too. The JVM is a **build-time-only** dependency: consumers of
generated code and the runtime never need Java at runtime, and Phase 6 wraps the JVM invocation so
users only ever type a CMake target / Bazel rule / CLI command. (Recorded as ADR-0001 in Phase 0.)

### 3.2a Reference implementation: smithy-rs

[smithy-rs](https://github.com/smithy-lang/smithy-rs) is the closest prior art — the only
official Smithy generator that ships **both a client and a server generator plus the runtime**,
which is exactly our shape. We will treat it as the reference implementation and consult it
before designing each subsystem, mapping its structure onto ours:

| smithy-rs (Kotlin/Rust) | smithy-cpp equivalent | Used in |
|---|---|---|
| `codegen-core` (SymbolProvider, `RustWriter`, protocol serde generators shared by client & server) | `smithy-cpp-codegen` core module, `CppWriter` | Phase 2 |
| `codegen-client` + `ClientProtocolTestGenerator` | `smithy-cpp-codegen-client` + client protocol-test generation | Phase 3 |
| `codegen-server` (routing, constraint traits, `ValidationException`, `ServerProtocolTestGenerator`) | `smithy-cpp-codegen-server` | Phase 4 |
| `codegen-client-test` / `codegen-server-test` (Gradle projects that generate code from a corpus of test models, then compile and `cargo test` it) | our generate→compile→run test harness over the fixture corpus | Phases 2–5 |
| `rust-runtime/` crates: `aws-smithy-types`, `aws-smithy-http`, `aws-smithy-runtime`, `aws-smithy-http-server` | `runtime/` modules: `smithy/core`, `smithy/http`, `smithy/client`, `smithy/server` | Phase 1 |

Concretely: its shared-core/client/server module split, its "generate real projects from test
models and run their tests in CI" strategy, its server constraint-validation design
(`ConstraintViolation` → structured `ValidationException`, RFC'd in smithy-rs), and its
protocol-test generators are all patterns we adopt rather than reinvent. Where we deliberately
diverge (e.g., golden-file tests in addition to compile-and-run tests, C++ error-handling
idioms instead of Rust `Result`), the divergence is recorded in the relevant design doc.

### 3.3 Initial protocol (decision)

**Start with `restJson1`** (HTTP bindings + JSON bodies): it exercises the full HTTP binding
surface (labels, query params, headers, payloads, status codes) that a server router needs, it has
a large official protocol-test suite (`smithy-aws-protocol-tests`), and it is the most common
choice for non-AWS Smithy services. `awsJson1.0` (RPC-style) follows in Phase 7 as the proof that
the protocol layer is genuinely pluggable.

### 3.4 Generated API shape (sketch, refined in Phase 2/3/4 design docs)

```cpp
// Client
smithy::ClientConfig cfg;
cfg.endpoint = "http://localhost:8080";
WeatherClient client{cfg};
auto outcome = client.GetForecast(GetForecastInput{.city = "Seattle"});
if (outcome) { use(outcome->chanceOfRain); } else { log(outcome.error().message()); }

// Server — user implements a generated pure-virtual handler interface
struct MyWeatherService final : WeatherServiceHandler {
  GetForecastOutcome GetForecast(const GetForecastInput& in, const RequestContext& ctx) override;
};
smithy::ServerConfig scfg{.port = 8080};
WeatherServiceServer server{std::make_shared<MyWeatherService>(), scfg};
server.serve();   // or server.start() / server.stop() for tests
```

---

## 4. Phases

Each phase lists **Goals**, **Tasks**, **Testing**, **Docs**, and **Exit criteria**. Phases are
sequential milestones, but small overlaps are expected (e.g., Phase 4 design can start while
Phase 3 stabilizes). Indicative effort assumes 1–2 engineers.

---

### Phase 0 — Foundations (≈1–2 weeks)

**Goals:** a repo where every later phase has rails: builds, CI, style, decisions recorded.

**Tasks**
- Repo layout as in §3.1; top-level `CMakeLists.txt` **and** `MODULE.bazel`/`BUILD` files
  (runtime builds under both from day one), Gradle build for codegen.
- Toolchain baseline: C++20; CMake ≥ 3.25; Bazel ≥ 7 (bzlmod-only); Java 17 + Gradle for
  codegen; GoogleTest via FetchContent / `@googletest`; nlohmann/json (or simdjson later)
  vendored behind the runtime's serde interface.
- CI (GitHub Actions):
  - Linux gcc + clang, macOS clang, Windows MSVC build matrix for the runtime (CMake).
  - **Bazel build+test job** (Linux + macOS) building the same runtime targets — CMake/Bazel
    parity is enforced by CI from the first commit, not retrofitted.
  - ASan/UBSan job on Linux clang.
  - Codegen: Gradle build + unit tests.
  - `clang-format` + `clang-tidy` + `.editorconfig` checks; Spotless for Java; `buildifier` for
    Bazel files.
- Author ADR-0001 (JVM generator, §3.2), ADR-0002 (restJson1 first, §3.3), ADR-0003 (error
  handling: `Outcome<T, Error>` return values, no exceptions across generated API), ADR-0004
  (dual build support: CMake + Bazel are co-equal, source layout stays build-system agnostic,
  every runtime/test target exists in both).
- Check in the **model corpus**: `examples/weather` (simple REST CRUD) and `examples/cafe`
  (unions, enums, streaming-payload placeholder, pagination) — these fixtures drive tests in every
  later phase.
- `CONTRIBUTING.md`, issue/PR templates, license.

**Testing:** CI green on all platforms with a hello-world runtime target and empty codegen module.

**Docs:** `README.md` with project vision + status table; ADRs; `docs/development.md` (how to
build/test both halves).

**Exit criteria:** a contributor can clone, build, and run (trivial) tests on all three OSes with
documented commands, via **either CMake or Bazel**; ADRs 1–4 merged.

---

### Phase 1 — Runtime core: `smithy-cpp-runtime` (≈3–4 weeks)

**Goals:** the hand-written C++ library that generated code will call into. No codegen yet — the
runtime is designed against *hand-written* mock "generated" code for the weather example, which
later becomes the golden reference for Phase 2/3 output. Module boundaries deliberately mirror
smithy-rs's runtime crates (§3.2a): before finalizing each module's API, review the corresponding
`aws-smithy-*` crate for the problems it already solved (e.g., `aws-smithy-types` for
timestamp/document edge cases, `aws-smithy-http-server` for router and rejection design). Every
module ships with both CMake and Bazel targets (ADR-0004).

**Tasks**
- **Core types** (`smithy/core`):
  - `Blob`, `Timestamp` (epoch-seconds / date-time / http-date parsing+formatting), `Document`
    (JSON-like dynamic value), `BigDecimal`/`BigInteger` placeholders.
  - `Outcome<T, E>` (expected-like), `Error` hierarchy: `ModeledError` base, transport errors,
    deserialization errors; error code + message + retryability metadata.
- **JSON serde** (`smithy/json`): thin reader/writer wrappers over the JSON backend with
  Smithy-specific behaviors (timestamps, blobs as base64, sparse vs dense collections, unknown
  member skipping for forward compatibility).
- **HTTP layer** (`smithy/http`):
  - Value types: `HttpRequest`, `HttpResponse`, `Headers` (case-insensitive), `Uri` with
    escaping rules matching Smithy httpLabel/httpQuery requirements.
  - Interfaces: `HttpClient` (async-capable: `send(request) -> future<Response>` plus sync
    convenience), `HttpServerTransport`.
  - Implementations: **libcurl** `HttpClient`; **Boost.Beast/asio** `HttpServerTransport`; plus
    an **in-memory loopback transport** that connects an `HttpClient` directly to a server request
    handler with no sockets — the backbone of fast integration tests later.
- **Client base** (`smithy/client`): `ClientConfig` (endpoint, timeouts, user-agent), interceptor
  hook points (before-send/after-receive), simple retry policy (off by default until Phase 7).
- **Server base** (`smithy/server`): `Router` (method + URI-pattern matching with literal >
  label precedence per Smithy spec), `RequestContext`, error→HTTP response mapping, and a
  `ValidationFailure` type for Phase 4's constraint checks.

**Testing**
- GoogleTest unit tests per module; target ≥90% line coverage on `core`, `json`, `http` value
  types (coverage job added to CI).
- URI/label/query escaping tested against the tables in the Smithy HTTP binding spec.
- Timestamp round-trip tests across all three formats, including edge cases (fractional seconds,
  pre-epoch).
- Loopback transport tested with a hand-written echo service.
- ASan/UBSan on all runtime tests in CI.

**Docs:** Doxygen set up and published (GitHub Pages) for the runtime API; `docs/runtime.md`
overview; each interface header carries usage examples.

**Exit criteria:** a **hand-written** weather client and server built on the runtime pass an
end-to-end request over both loopback and real sockets in CI. This hand-written pair is the
prototype the generators must reproduce.

---

### Phase 2 — Codegen skeleton + type generation (≈3–4 weeks)

**Goals:** the smithy-build plugin exists; from a model it generates a compiling CMake project
containing all **data types** — no operations yet.

**Tasks**
- `CppCodegenPlugin` implementing `SmithyBuildPlugin` (`"cpp-codegen"` in `smithy-build.json`),
  wired through `DirectedCodegen`: `CppSettings` (namespace, module name, client/server/both),
  `CppSymbolProvider`, `CppWriter` (a `SymbolWriter` with `#include` management and
  forward-declaration handling), `CppDelegator`. Before writing each piece, review the
  smithy-rs `codegen-core` counterpart (`RustSymbolProvider`, `RustWriter`, its
  `CodegenDecorator` extension mechanism) and adopt its structure where it transfers.
- **Symbol provider**: shape → C++ mapping (structure→struct, string enum→`enum class` +
  unknown-value preservation, union→`std::variant` wrapper with visitor helpers, list→`std::vector`,
  map→`std::map`/`unordered_map`, optionality via `std::optional`, recursive shapes via
  `std::unique_ptr` indirection, C++ reserved-word and keyword escaping, PascalCase/camelCase
  conventions documented and fixed here).
- Generate: headers + sources for all shapes in closure, **both** a `CMakeLists.txt` and a
  `BUILD.bazel` for the generated module (links `smithy-cpp-runtime`), equality operators, and
  builder-style construction (designated initializers where possible).
- Deterministic output (stable ordering) — a hard requirement for golden tests.
- Stand up the **generate→compile→run test pipeline** modeled on smithy-rs's
  `codegen-client-test`/`codegen-server-test`: a Gradle task generates projects from every
  fixture model and drives their C++ builds+tests; this pipeline is what Phases 3–5 extend.

**Testing**
- **Golden tests**: for each fixture model, generated files are compared byte-for-byte against
  checked-in expected output (`codegen-tests/golden/…`); a Gradle task regenerates goldens
  deliberately.
- **Compile tests in CI**: generate from the fixture corpus, then configure+build the generated
  CMake projects with the runtime — generation isn't "done" until the output compiles warning-free
  (`-Wall -Wextra -Werror`) on the full platform matrix.
- Java unit tests for symbol provider edge cases (reserved words, deep recursion, name
  collisions across namespaces).

**Docs:** `docs/design/codegen-architecture.md`; `docs/generated-types.md` (what users get per
Smithy shape — the naming/mapping contract).

**Exit criteria:** `smithy build` on the weather and cafe models emits type libraries that compile
cleanly on all CI platforms; golden tests lock the output.

---

### Phase 3 — Client generation, restJson1 (≈4–5 weeks)

**Goals:** generated clients are functional and protocol-compliant: users can call a real service.

**Tasks**
- Operation method generation on `<Service>Client`: sync `Outcome`-returning methods first;
  async (`std::future`-based) variants once sync is stable.
- **restJson1 request serialization**: httpLabel (greedy + normal), httpQuery/QueryParams,
  httpHeader/PrefixHeaders, httpPayload (shape and blob payloads), body member serialization,
  content-type and accept headers, idempotency token auto-fill.
- **Response deserialization**: status-code handling, header/payload/body unbinding, streaming
  blob payload as `std::istream`-like body.
- **Error deserialization**: error discriminator handling (`X-Amzn-Errortype` header / `__type` /
  `code` field per the restJson1 spec), generated modeled-error types, unknown-error fallback;
  errors surfaced through `Outcome::error()` with `ErrorsAs<T>()` accessors.
- Client plumbing: endpoint resolution from config, per-operation timeout override, interceptors
  invoked at generated call sites.
- **Protocol test generation**: generate GoogleTest cases from `smithy.test#httpRequestTests` and
  `#httpResponseTests` traits, run against the official `smithy-aws-protocol-tests` restJson1
  suite with a capturing mock `HttpClient`. Model this on smithy-rs's
  `ClientProtocolTestGenerator`, including its pattern of a checked-in, must-shrink **exclusion
  list** (`FailingTest`/broken-test annotations) for known-failing cases.

**Testing**
- Full restJson1 client protocol-test suite in CI (minus documented exclusions).
- Golden + compile tests extended to client output.
- Hand-written smoke test: generated weather client vs. the Phase 1 *hand-written* server —
  the first half of the client↔server bridge.
- Unit tests for serde helpers added to the runtime along the way.

**Docs:** `docs/client-guide.md` (quick start: model → client → first call), error-handling
guide, `examples/weather/client-demo/` runnable example.

**Exit criteria:** generated weather client passes protocol tests and talks to the hand-written
server over real sockets in CI; exclusion list reviewed and justified line-by-line.

---

### Phase 4 — Server generation, restJson1 (≈4–5 weeks)

**Goals:** generated servers are functional: routing, deserialization, validation, handler
dispatch, response serialization.

**Tasks**
- Generate `<Service>Handler` pure-virtual interface (one method per operation, `Outcome`
  returns so handlers report modeled errors without exceptions) and `<Service>Server` that binds
  the handler to the runtime router + a `HttpServerTransport`.
- **Request deserialization** (mirror of Phase 3 serialization) and **response serialization**
  (mirror of Phase 3 deserialization) — factor shared serde generation so client and server reuse
  one serializer-generator with direction flipped.
- **Routing**: generated route table (method, URI pattern, greedy labels) registered with the
  runtime router; 404/405 handling; `Content-Type` checking (415).
- **Constraint validation** from traits: `@required`, `@length`, `@range`, `@pattern`,
  `@uniqueItems`, enum membership — failures produce a structured 400 `ValidationException`
  *before* user handler code runs. Follow smithy-rs's server design here directly: its
  constraint-traits RFC, `ConstraintViolation` types, and `smithy.framework#ValidationException`
  wiring are the precedent, translated to C++ idioms.
- Error mapping: modeled errors → status + restJson1 error body; unmodeled/handler panic → 500
  with safe (non-leaking) body.
- Server-side **protocol tests**: generate tests from `httpRequestTests` (feed the wire request,
  assert parsed input) and `httpMalformedRequestTests` (assert 400s) — the malformed-request
  suite is the server's validation conformance test. smithy-rs's `ServerProtocolTestGenerator`
  is the template.

**Testing**
- Server protocol-test + malformed-request suites in CI with exclusion list.
- Golden + compile tests extended to server output.
- Inverse smoke test: Phase 1 hand-written client vs. **generated** server.
- Router unit tests: precedence, greedy labels, ambiguous-route detection at generation time
  (generator emits an error, with a Java unit test proving it).

**Docs:** `docs/server-guide.md` (implementing a handler, running a server, deployment notes),
validation behavior reference.

**Exit criteria:** generated weather server passes server-side protocol tests; hand-written
client round-trips against it in CI.

---

### Phase 5 — Client↔server integration testing (≈2–3 weeks)

**Goals:** the headline requirement: **generated clients integration-test generated servers**,
systematically and in CI, for every fixture model.

**Tasks**
- **Integration harness** (`tests/integration/`), an extension of the Phase 2
  generate→compile→run pipeline (smithy-rs's `codegen-client-test`/`codegen-server-test`
  pattern, taken one step further by wiring the two generated halves together):
  - For each example model, generate *both* client and server, compile them into one test binary
    with a reference handler implementation, and run GoogleTest suites that call every operation
    through the generated client against the generated server.
  - Two transport modes, same test body: (a) **loopback** (in-memory, fast, runs everywhere,
    ASan-friendly) and (b) **real sockets** on an ephemeral port (catches transport/framing bugs).
- **Round-trip coverage matrix**: auto-derive from the model a test per operation covering:
  minimal input (only required members), maximal input (all members set), boundary values for
  constrained members, every modeled error (handler triggers it, client must surface the right
  typed error), unknown-member tolerance (server adds an extra JSON field; old client must ignore
  it).
- **Property-based round-trip tests** (rapidcheck or handwritten generators): random valid inputs
  → client serialize → server deserialize → echo handler → client deserialize → compare with
  `operator==`. This is the single highest-value test for serde symmetry.
- Grow the fixture corpus to force coverage: a model exercising unions + recursive shapes, one
  with all timestamp formats and http bindings, one with pagination, one with only error cases.
- Nightly CI job running the full matrix with sanitizers; PR CI runs the loopback subset.

**Testing:** this phase *is* testing; its own correctness is guarded by mutation checks
(deliberately break a serializer locally → harness must fail).

**Docs:** `docs/design/integration-testing.md` — how the harness works, how to add a fixture
model, how failures are triaged.

**Exit criteria:** every fixture model has a generated-client-vs-generated-server suite green in
CI on Linux/macOS/Windows; a seeded serde bug is demonstrably caught by the harness.

---

### Phase 6 — Ease of use: tooling, packaging, docs (≈3–4 weeks)

**Goals:** a newcomer goes from empty directory to running client+server in under 15 minutes
without reading generator internals or touching Gradle.

**Tasks**
- **CMake integration**: `find_package(SmithyCpp)` +
  `smithy_cpp_generate(TARGET weather_client MODEL weather.smithy MODE CLIENT)` which invokes the
  generator (bootstrapping the pinned JVM tooling via the Smithy CLI), wires generated sources
  into the build, and sets up dependencies so edits to `.smithy` files trigger regeneration.
- **First-class Bazel integration** (`bazel/` + `MODULE.bazel`):
  - Rules: `smithy_cpp_client_library`, `smithy_cpp_server_library`, and `smithy_cpp_types_library`
    (attrs: `model`/`srcs` for `.smithy` files + model deps, `namespace`, protocol options),
    each producing an ordinary `cc_library` consumers depend on like any other target.
  - The generator runs as a Bazel **toolchain/action** with a hermetic JVM via `rules_java` —
    generation happens inside the build graph (correct caching, remote-execution compatible),
    never as a "run this script first" step.
  - bzlmod module `smithy_cpp` published to the **Bazel Central Registry**; runtime targets
    (`@smithy_cpp//runtime:core`, `:client`, `:server`, …) consumable directly.
  - Out-of-tree consumer example (`examples/bazel-consumer/`) exercised in CI: a standalone
    workspace that depends on the released module, defines a model, builds client + server, and
    runs the Phase-5-style integration test — this is the Bazel quick-start acceptance test.
- **CLI wrapper**: `smithy-cpp generate --model … --mode client|server|both --out …` (thin wrapper
  around `smithy build` with our plugin preconfigured) for users of neither build system;
  distributed via the Smithy CLI's plugin mechanism.
- **Project templates**: `smithy-cpp init [--bazel|--cmake]` / template repos producing the
  canonical layout (model/, client/, server/, integration-test skeleton *pre-wired to test the
  server with the client*, mirroring our Phase 5 harness — users inherit the pattern for free)
  in both build systems.
- **Packaging**: vcpkg port and Conan recipe for the runtime; Bazel Central Registry module;
  pinned generator distribution via Maven Central; document a FetchContent path for people who
  want none of the above.
- **Docs site** (mkdocs-material or Docusaurus, published from CI): Quick Start, Client Guide,
  Server Guide, Testing Your Service, Generated Code Reference, Customization (namespaces,
  naming), Troubleshooting, FAQ. Every code snippet in the docs is extracted from compiled,
  CI-tested example code — no drifting snippets.
- Error-message audit: every generator diagnostic names the shape, the trait, and a fix.

**Testing**
- **Doc-driven acceptance tests in CI**: workflows that literally follow the Quick Start from a
  clean container (install, init, generate, build, run integration test) — **one for CMake, one
  for Bazel** — if either tutorial breaks, CI fails.
- CMake function tested against out-of-tree consumer projects (FetchContent and installed-package
  modes); Bazel rules tested against the out-of-tree consumer workspace across the last two
  Bazel LTS releases.

**Docs:** the docs site is the deliverable; Quick Start, Testing, and Customization pages have
parallel CMake and Bazel tracks of equal depth.

**Exit criteria:** both quick-start acceptance tests green in CI; an external tester (someone not
on the project) completes each tutorial without help; vcpkg/Conan/BCR packaging validated in CI.

---

### Phase 7 — Hardening & 0.1.0 release (≈4–6 weeks)

**Goals:** production-readiness features, robustness work, second protocol as a pluggability
proof, and a cut release.

**Tasks**
- **Client robustness**: retry policy (exponential backoff + jitter, honoring `@retryable`),
  connection pooling in the curl transport, request timeouts end-to-end, cancellation.
- **Server robustness**: thread-pool tuning knobs, graceful shutdown/drain, request size limits,
  slow-client timeouts, structured logging hooks, metrics hooks (request count/latency callbacks).
- **Auth hooks**: `@httpBearerAuth` / `@httpApiKeyAuth` support — client-side credential
  providers, server-side authenticator interface (SigV4 explicitly deferred).
- **Pagination**: generated paginator iterators from `@paginated`.
- **Second protocol — `awsJson1.0`**: implement behind the protocol-generator interface; its
  protocol tests + a full Phase-5-style integration matrix prove the abstraction. Any refactor
  needed here is the cheapest it will ever be.
- **Fuzzing**: libFuzzer harnesses for JSON deserialization, URI parsing, and the server's
  request parsing (fed by the malformed-request corpus); OSS-Fuzz application once stable.
- **Performance**: benchmark suite (Google Benchmark) for serde and request throughput; publish
  numbers; no-regression check in CI (informational at first).
- **Release engineering**: semantic versioning policy, generated-code compatibility policy
  (what a generator upgrade may change), CHANGELOG discipline, signed tags, versioned docs;
  cut **v0.1.0** of runtime + generator together.

**Testing:** everything above lands with tests; fuzzers run nightly; full integration matrix
(both protocols × all fixtures × loopback+sockets × 3 OSes) green for release.

**Docs:** production guide (timeouts, retries, shutdown), upgrade/compatibility policy,
benchmark methodology.

**Exit criteria:** v0.1.0 tagged and published (vcpkg/Conan/Maven), with the release gated on
the full test matrix and the quick-start acceptance test.

---

## 5. Testing strategy (summary)

| Layer | What | Where it lands |
|---|---|---|
| Runtime unit tests | GoogleTest on core/json/http/client/server modules, ≥90% coverage on core | Phase 1, continuous |
| Codegen unit tests | Java tests for symbol provider, naming, route-conflict detection | Phase 2+ |
| Golden (snapshot) tests | Byte-exact expected generated output per fixture model | Phase 2+ |
| Compile tests | Generated code must build `-Werror` on Linux/macOS/Windows | Phase 2+ |
| Protocol compliance | Generated tests from Smithy `httpRequestTests`/`httpResponseTests`/`httpMalformedRequestTests` (client- and server-side), official restJson1 suite | Phases 3–4 |
| **Client↔server integration** | **Generated client exercises generated server** per fixture: coverage matrix + property-based round-trips, loopback + real sockets | Phase 5, continuous |
| Doc acceptance | CI follows the Quick Start verbatim from a clean environment | Phase 6 |
| Fuzzing & sanitizers | ASan/UBSan on all C++ tests; libFuzzer on parsers, nightly | Phase 0 (sanitizers), Phase 7 (fuzzing) |
| Benchmarks | Serde + throughput, regression-tracked | Phase 7 |

---

## 6. CI matrix (steady state)

- **Per-PR:** Linux gcc/clang + macOS + Windows CMake builds; Linux + macOS Bazel builds+tests;
  runtime unit tests; codegen tests + goldens; compile tests; protocol tests; loopback
  integration suite; lint/format (incl. buildifier).
- **Nightly:** full integration matrix (sockets + sanitizers), fuzzers, coverage report,
  benchmark run, CMake + Bazel doc acceptance tests, out-of-tree Bazel consumer across
  supported Bazel LTS versions.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| JVM dependency deters C++ users | Build-time only; hidden behind CMake function/CLI (Phase 6); documented clearly in ADR-0001 |
| Protocol edge cases (timestamps, escaping, greedy labels) | Lean on official protocol-test suites from day one; exclusion lists are visible, reviewed, and must shrink |
| Serde asymmetry between client and server | Single shared serde generator with direction flipped (Phase 4); property-based round-trip tests (Phase 5) |
| Golden tests become churn-heavy | Deterministic output requirement + one-command regeneration + goldens reviewed as normal diffs |
| C++ dependency pain (curl/Beast) for consumers | Transports isolated behind interfaces; header-only loopback needs no deps; vcpkg/Conan/BCR handle the rest |
| Dual build-system (CMake + Bazel) maintenance drift | ADR-0004: build-system-agnostic source layout, every target exists in both, parity enforced by per-PR CI from Phase 0 |
| Windows quirks (winsock, MSVC conformance) | Windows in the CI matrix from Phase 0, not retrofitted |
| Scope creep toward AWS SDK features | Non-goals section + ADRs; auth limited to bearer/api-key in 0.1 |

---

## 8. Deferred beyond 0.1.0

Event streams (`@streaming` unions), HTTP/2 & TLS configuration surface, `awsQuery`/`restXml`
protocols, SigV4/AWS traits, waiters, client-side compression/checksums traits, MQTT bindings,
C++ modules support, async server handler API (coroutines) — each gets an issue and a design doc
before implementation.

## 9. Open questions (to resolve in early ADRs)

1. JSON backend: nlohmann (ergonomics) vs simdjson+writer (speed) — start nlohmann behind the
   serde interface, benchmark in Phase 7 (ADR in Phase 1).
2. Async model for 0.1: `std::future` now, coroutine (`co_await`) client in a later minor —
   confirm in Phase 3 design.
3. Minimum compiler floor: proposal gcc 11 / clang 14 / MSVC 19.30 — confirm in Phase 0.
4. Whether server transport ships Beast-only or also a plain-asio minimal HTTP/1.1 impl to cut
   the Boost dependency — decide in Phase 1 after prototyping.

---

## 10. Milestone summary

| Phase | Outcome | Indicative duration |
|---|---|---|
| 0 | Repo, CI, ADRs, fixture models | 1–2 wk |
| 1 | Runtime library; hand-written client+server round-trip | 3–4 wk |
| 2 | Plugin + type generation, golden & compile tests | 3–4 wk |
| 3 | Generated client, restJson1, protocol tests | 4–5 wk |
| 4 | Generated server, validation, protocol tests | 4–5 wk |
| 5 | Generated-client ↔ generated-server integration harness | 2–3 wk |
| 6 | CMake + Bazel rules/UX, CLI, packaging (vcpkg/Conan/BCR), docs site, quick-start acceptance tests | 3–4 wk |
| 7 | Hardening, second protocol, fuzzing, **v0.1.0** | 4–6 wk |

Total: roughly 6–8 months of focused work for a small team, with a usable
generated-client-tests-generated-server demo available from the end of Phase 5.
