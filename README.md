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
| 1 | C++ runtime library (`smithy/core`, `json`, `cbor`, `http`, `client`, `server`) | ✅ Core done ([docs/runtime.md](docs/runtime.md)); Boost.Beast production server transport is next (ADR-0006) |
| 2 | Codegen plugin + type generation | **In progress** — types generated for both fixtures, golden+compile+behavior tested ([docs/design/codegen-architecture.md](docs/design/codegen-architecture.md)) |
| 3 | Client generation (restJson1 + rpcv2Cbor) | Not started |
| 4 | Server generation (restJson1 + rpcv2Cbor) | Not started |
| 5 | Generated-client ↔ generated-server integration harness | Not started |
| 6 | Bazel rules, CLI, packaging (BCR + Maven Central), docs site | Not started |
| 7 | Hardening, fuzzing, v0.1.0 | Not started |
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
