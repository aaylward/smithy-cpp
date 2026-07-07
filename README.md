# smithy-cpp

Smithy code generators for C++ — generate idiomatic C++ clients and servers from
[Smithy](https://smithy.io) models, plus the shared C++ runtime they build on.

- **Vendor-neutral:** implements Smithy and its protocol specs; nothing AWS-specific.
- **Two protocols from the start:** `restJson1` (REST) and `rpcv2Cbor` (RPC).
- **Bazel-native:** Bazel 9 is the sole supported build system for the repo and consumers.
- **Client tests server:** generated clients integration-test generated servers in CI.

See [`docs/PLAN.md`](docs/PLAN.md) for the full phased plan and
[`docs/adr/`](docs/adr/) for architecture decisions.

## Status

| Phase | Scope | Status |
|---|---|---|
| 0 | Foundations: Bazel 9 workspace, codegen skeleton, CI, ADRs, fixture models | ✅ Done |
| 1 | C++ runtime library (`smithy/core`, `json`, `cbor`, `http`, `client`, `server`) | ✅ Done ([docs/runtime.md](docs/runtime.md)) incl. Boost.Beast production server transport (ADR-0006) |
| 2 | Codegen plugin + type generation | ✅ Done — types generated for both fixtures, golden+compile+behavior tested ([docs/design/codegen-architecture.md](docs/design/codegen-architecture.md)) |
| 3 | Client generation (restJson1 + rpcv2Cbor) | ✅ Done — serde + clients for both protocols with typed errors ([docs/generated-types.md](docs/generated-types.md)); ~240 official protocol conformance cases green (documented exclusions) |
| 4 | Server generation (restJson1 + rpcv2Cbor) | ✅ Done — handlers, routing, serde, all HTTP bindings incl. `@httpPayload`/`@httpPrefixHeaders`, constraint validation, parser strictness, content negotiation; ~1,175 official conformance cases green ([docs/server-guide.md](docs/server-guide.md)) |
| 5 | Generated-client ↔ generated-server integration harness | ✅ Done — every fixture ships a generated integration suite: seeded random round-trips over loopback and real sockets, per-error mapping, unknown-member tolerance, mutation-checked ([docs/design/integration-testing.md](docs/design/integration-testing.md)) |
| 6 | Bazel rules, CLI, packaging (BCR + Maven Central), docs site | 🔨 In progress — `smithy_cpp_{types,client,server}_library` rules run the generator hermetically inside the build graph, out-of-tree consumer module tested in CI, CLI via `bazel run //codegen:generator` ([docs/quickstart.md](docs/quickstart.md)); BCR/Maven publishing deferred until production validation; docs site pending |
| 7 | Hardening, fuzzing, v0.1.0 | 🔨 In progress — retries with full-jitter exponential backoff, gzip `@requestCompression` (client + server), consumer CI across linux/macos/windows ([docs/production-guide.md](docs/production-guide.md)) |
| 8 | Bidirectional streaming (event streams, WebSockets) | Not started |

## Building

```sh
bazel test //...                     # C++ runtime (requires bazelisk)
cd codegen && gradle build           # Smithy → C++ generator (JVM)
```

See [`docs/development.md`](docs/development.md) for details and
[`CONTRIBUTING.md`](CONTRIBUTING.md) for contribution guidelines.

## License

[Apache 2.0](LICENSE)
