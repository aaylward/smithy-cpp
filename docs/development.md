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

```sh
# C++ runtime: build + run all tests
bazel test //...

# With sanitizers (clang recommended: CC=clang CXX=clang++)
bazel test //... --config=asan --config=ubsan

# Codegen: build + unit tests + format check
cd codegen && gradle build spotlessCheck
```

## Benchmarks

```sh
bazel run -c opt //benchmarks:serde_benchmark      # Document pivot + JSON/CBOR codecs
bazel run -c opt //benchmarks:request_benchmark    # per-protocol client<->server round trips
```

CI runs both as an informational job (no pass/fail threshold yet — the
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
- Machine-specific Bazel flags go in `.bazelrc.user` (gitignored), e.g. a
  `--downloader_config` when working behind a proxy that blocks GitHub downloads.
- The Boost-dependent targets (`//runtime:http_beast` and the tests that use it) fetch ~30
  modular Boost archives. Behind a proxy that blocks GitHub they won't fetch; exclude them and
  run everything else with
  `bazel test //... -- -//runtime:http_beast -//runtime:beast_transport_test -//examples/weather:weather_e2e_beast_test`
  (the Beast code can still be exercised against system Boost headers with plain g++).
