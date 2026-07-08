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

## What works today (v0.1.0)

- **Clients and servers for all three protocols**, each green against a conformance suite in CI
  (the official alloy and rpcv2Cbor suites, an authored one for jsonRpc2) with documented,
  must-shrink exclusion lists.
- **The full generated surface:** typed structs/unions/enums, serde, HTTP bindings, constraint
  validation with suite-exact `ValidationException` output (ReDoS-safe `@pattern`), typed
  modeled errors, paginators, idempotency tokens, and gzip request compression.
- **In-graph generation:** `smithy_cpp_{types,client,server}_library` run the generator
  hermetically inside the Bazel build graph — no scripts, no JVM to install. An out-of-tree
  consumer module is CI-tested on Linux/macOS/Windows, plus a CLI for generating elsewhere.
- **Production serving and calling** over Boost.Beast with TLS in both directions, retries with
  jittered backoff, client interceptors, server middleware, and bearer/API-key auth wiring.
- **Hardening in CI:** sanitizer jobs, libFuzzer harnesses, hostile-input test banks, and every
  fixture's generated client integration-testing its generated server.

Not yet: bidirectional streaming (event streams/WebSockets) and Bazel Central Registry / Maven
publishing — consumers pin a git commit for now. Roadmap and per-phase status live in
[`docs/PLAN.md`](docs/PLAN.md).

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

See [`docs/development.md`](docs/development.md) for details and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines.

## License

[Apache 2.0](LICENSE)
