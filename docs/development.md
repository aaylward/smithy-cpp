# Development guide

Two build trees live in this repository (see PLAN §3.1):

| Tree | Language | Build | What it is |
|---|---|---|---|
| `runtime/` (+ future generated code, examples, integration tests) | C++20 | Bazel 9 | The `smithy-cpp-runtime` library that generated code links against |
| `codegen/` | Java 17 | Gradle | The Smithy → C++ generator (smithy-build plugin) |

## Prerequisites

- [bazelisk](https://github.com/bazelbuild/bazelisk) (reads `.bazelversion`, currently `9.x`)
- JDK 17+ and Gradle 8.14+ (or use `gradle/actions/setup-gradle` versions in CI)
- `clang-format` / `clang-tidy` (LLVM 18+) and `buildifier` for linting

## Building and testing

One command verifies everything the CI gate checks (bazel tests, gradle
build + format, golden freshness, lint):

```sh
make verify        # what CI gates a PR on
make verify-full   # + sanitizers, fuzzer smoke runs, the consumer module, clang-tidy
```

Each aggregate is also callable piecemeal (`make test codegen goldens lint
sanitize fuzz-smoke consumer tidy coverage benchmarks format`); the recipes
mirror `.github/workflows/ci.yml`, one target per job. The underlying
commands:

```sh
# C++ runtime: build + run all tests
bazel test //...

# With sanitizers (clang recommended: CC=clang CXX=clang++)
bazel test //... --config=asan --config=ubsan

# Codegen: build + unit tests + format check
cd codegen && gradle build spotlessCheck
```

The Java suite asserts on generated-source substrings; the compile-the-output
harness under `codegen/compile-tests/` is what proves hostile-but-legal models
(keyword member names, quote/backslash enum values, int64-extreme bounds —
issue #43's class) still *compile*: it runs the generator inside the Bazel
graph for every protocol, client and server mode both, and builds the result.
Extend its `model/gauntlet.smithy` when adding a new escaping/naming rule.

## Benchmarks

```sh
bazel run -c opt //benchmarks:serde_benchmark      # Document pivot + JSON/CBOR codecs
bazel run -c opt //benchmarks:request_benchmark    # per-protocol client<->server round trips
bazel run -c opt //benchmarks:beast_benchmark      # real-TCP transports: socket, Beast, Beast TLS
```

CI runs all three as an informational job (no pass/fail threshold yet — the
numbers establish the baseline; see PLAN Phase 7).

## Formatting and linting

```sh
# C++
find runtime -name '*.h' -o -name '*.cc' | xargs clang-format -i

# Starlark (BUILD/MODULE/bzl)
buildifier --lint=warn -r .

# Java
cd codegen && gradle spotlessApply
```

## Repository conventions

- Runtime headers live under `runtime/include/smithy/<module>/…` and are included as
  `#include "smithy/<module>/….h"`.
- Every `cc_*` target loads rules from `@rules_cc//cc:defs.bzl` explicitly; the
  `--incompatible_autoload_externally` setting in `.bazelrc` exists only for third-party
  dependencies whose BUILD files predate Bazel 9's removal of the native C++ rules.
- Fixture Smithy models live under `examples/<name>/model/`; they double as test fixtures for
  codegen and the client↔server integration harness (PLAN Phases 2–5).
- Generated output under `examples/*/generated/` and `protocol-tests/*/generated/` is checked
  in as goldens: regenerate with `cd codegen && gradle generateFixtures generateProtocolTests`
  and commit model + output together — CI fails on drift. The drift check alone would let a
  generator bug ratify itself (both sides of the diff come from the same generator), so
  `GoldenProtocolTestAuditTest` additionally cross-checks the protocol-test goldens against the
  upstream suite definitions: every upstream case must be generated or excluded, wire facts must
  match, and exclusions must name real cases. The full loop (including the
  consumer-side story) is in [model-evolution.md](model-evolution.md).
- Goldens ratify generated-code *shape*, they don't judge it: redundant or dead emission
  compiles, passes conformance, and lives in the goldens until a human reads them (issue #68).
  Convention: every fix for a "generator emitted redundant/dead code" bug lands with an
  exactly-once or absence assertion — in `GeneratedCodeShapeTest`, or beside the feature's own
  tests like `HttpJsonBindingProtocolTest.serverWritesResponsePrefixHeadersExactlyOnce` — so the
  smell cannot quietly return. Placement rule for branch pins generally: a pin lands in the
  feature-owning test class when one exists (the compression-scoping pin lives in
  `BuildFileGeneratorTest`); `ConditionalWiringCoverageTest` is the fallback for arms with no
  owning class, not the default destination.
- Machine-specific Bazel flags go in `.bazelrc.user` (gitignored), e.g. a
  `--downloader_config` when working behind a proxy that blocks GitHub downloads. A rewrite of
  `github.com/(.*)` to `mirror.bazel.build/github.com/$1` unblocks most module archives; for the
  few that aren't mirrored (nlohmann_json, google_benchmark), `git clone` the exact tag (git often
  works where archive downloads don't) and point `--override_module=<name>=<checkout>` at it —
  nlohmann ships its own BUILD.bazel, so only the BCR MODULE.bazel patch is needed. Add
  `--lockfile_mode=off` so local runs don't dirty the checked-in MODULE.bazel.lock.
- The Boost-dependent targets (`//runtime:http_beast` and the tests that use it) fetch ~30
  modular Boost archives. Behind a proxy that blocks GitHub they won't fetch; exclude them and
  run everything else with
  `bazel test //... -- -//runtime:http_beast -//runtime:beast_transport_test -//examples/weather:weather_e2e_beast_test`
  (the Beast code can still be exercised against system Boost headers with plain g++).
