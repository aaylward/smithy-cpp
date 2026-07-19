# smithy-cpp

Smithy code generators for C++ — generate idiomatic C++ clients and servers from
[Smithy](https://smithy.io) models, plus the shared C++ runtime they build on.

**Start here → [docs/quickstart.md](docs/quickstart.md):** empty directory to a generated C++
client integration-testing a generated C++ server, in one Bazel module — no prior Smithy
experience assumed. Day 2 (evolving the model) is
[docs/model-evolution.md](docs/model-evolution.md).

- **Vendor-neutral:** implements Smithy and its protocol specs; nothing AWS-specific. The REST
  protocol is [`alloy#simpleRestJson`](https://disneystreaming.github.io/smithy4s/docs/protocols/simple-rest-json/overview/)
  (the neutral protocol smithy4s uses — so smithy-cpp clients and smithy4s services interoperate).
- **Three protocols:** `alloy#simpleRestJson` (REST/JSON), `smithy.protocols#rpcv2Cbor` (RPC/CBOR),
  and `smithy.cpp.protocols#jsonRpc2` (RPC/JSON over JSON-RPC 2.0) — all vendor-neutral.
- **Bazel-native:** Bazel 9 is the sole supported build system for the repo and consumers.
- **Client tests server:** generated clients integration-test generated servers in CI.

## What works today (0.1.0-dev, untagged)

- **Clients and servers for all three protocols**, each green against a conformance suite in CI
  (the official alloy and rpcv2Cbor suites, an authored one for jsonRpc2) with documented,
  must-shrink exclusion lists.
- **The full generated surface:** typed structs/unions/enums, serde, HTTP bindings, constraint
  validation with suite-exact `ValidationException` output (ReDoS-safe `@pattern`), typed
  modeled errors, paginators, idempotency tokens, and gzip request compression.
- **In-graph generation:** `smithy_cpp_{types,client,server}_library` run the generator
  hermetically inside the Bazel build graph — no scripts, no JVM to install. An out-of-tree
  consumer module is CI-tested on Linux/macOS (Windows was dropped in ADR-0008), plus a CLI for
  generating elsewhere.
- **Production serving and calling** over Boost.Beast with TLS in both directions, retries with
  jittered backoff, client interceptors, server middleware, and bearer/API-key auth wiring.
- **Hardening in CI:** sanitizer jobs, libFuzzer harnesses, hostile-input test banks, and every
  fixture's generated client integration-testing its generated server.

## Current limitations

Consolidated in one place — if your API depends on any of these, check here before adopting:

- **`@streaming` is not modeled yet.** The trait is ignored: a `@streaming` member generates
  as its plain shape (a streaming blob payload becomes an ordinary `smithy::Blob`, fully
  buffered in memory), and there is no event-stream or WebSocket transport. Streaming — blob
  streams and event streams, client and server — is [PLAN Phase 8](docs/PLAN.md), landing
  wire-format-first: the event-stream framing codec (`//runtime:eventstream`) is in;
  transports and the streaming API follow ([ADR-0014](docs/adr/0014-event-stream-framing-first.md)).
- **No Bazel Central Registry / Maven publishing** — consumers pin a git commit
  ([quickstart](docs/quickstart.md)); publishing is deferred until the project is
  production-validated (#44 tracks release readiness).
- **Linux and macOS only** ([ADR-0008](docs/adr/0008-drop-windows-support.md) dropped Windows).

Roadmap and per-phase status live in [`docs/PLAN.md`](docs/PLAN.md).

## Documentation

| Doc | What it covers |
|---|---|
| [quickstart.md](docs/quickstart.md) | Model → generated client + server, from an empty directory |
| [model-evolution.md](docs/model-evolution.md) | Day 2: changing the model, regeneration, drift detection |
| [generated-types.md](docs/generated-types.md) | The Smithy → C++ mapping contract |
| [server-guide.md](docs/server-guide.md) | What the generated server does before/after your handler |
| [production-guide.md](docs/production-guide.md) | Real transports, TLS, retries, auth, middleware |
| [runtime.md](docs/runtime.md) | The `smithy-cpp-runtime` library, module by module |
| [development.md](docs/development.md) | Building, testing, and linting this repo |
| [versioning.md](docs/versioning.md) | Compatibility policy; [CHANGELOG.md](CHANGELOG.md) has releases |
| [PLAN.md](docs/PLAN.md) | The phased roadmap; [adr/](docs/adr) records architecture decisions |
| [design/](docs/design) | Internals: codegen architecture, the integration-test harness; [fuzzing.md](docs/fuzzing.md) covers the fuzz setup |

## Building

```sh
bazel test //...                     # C++ runtime (requires bazelisk)
cd codegen && gradle build           # Smithy → C++ generator (JVM)
```

Bazel runs through [bazelisk](https://github.com/bazelbuild/bazelisk), which reads
[`.bazelversion`](.bazelversion) and fetches the pinned release. The first build downloads
the toolchain and all dependencies — see the quickstart's
[first-build section](docs/quickstart.md#the-first-build-cost-caching-and-locked-down-networks)
for what to expect and for proxy/offline setups.

See [`docs/development.md`](docs/development.md) for details and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines.

## License

[Apache 2.0](LICENSE)
