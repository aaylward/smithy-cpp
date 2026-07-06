# Codegen architecture

The generator (`codegen/smithy-cpp-codegen`, Java 17) is a smithy-build plugin named
`cpp-codegen`, built on `smithy-codegen-core`'s DirectedCodegen framework (ADR-0001), mirroring
smithy-rs's `codegen-core` structure (PLAN §3.2a).

## Pieces

| Class | Role |
|---|---|
| `CppCodegenPlugin` | `SmithyBuildPlugin` entry point; validates settings, rejects (for now) recursive shapes with a clear error, drives `CodegenDirector` |
| `CppSettings` | Plugin settings: `service`, C++ `namespace`, `runtimeTarget` (Bazel label of `//runtime:core` / `@smithy_cpp//runtime:core`) |
| `CppSymbolProvider` | Shape → C++ type mapping (docs/generated-types.md). A `Symbol`'s name is the full C++ type text; required `#include`s ride along in a symbol property |
| `CppWriter` | `SymbolWriter` per generated file: collects includes while the body is written, renders header comment + `#pragma once` + sorted includes + namespace wrapper. Byte deterministic |
| `DirectedCppCodegen` | Implements `DirectedCodegen`; Phase 2 handles structure/error/union/enum/intEnum directives and emits the module BUILD file from the service directive |
| `TypeGenerators` | The actual C++ emission for data shapes |
| `CppCodegenRunner` | CLI main used by the `generateFixtures` Gradle task (generation without smithy-build) |

Shapes are generated in topological order (a DirectedCodegen guarantee), so the single
`include/<ns>/types.h` needs no forward declarations. Recursive shapes are rejected at the door
until boxed-recursion support lands (tracked for Phase 3).

## Golden strategy: checked-in generated fixtures

Instead of separate golden files, the generated output for the fixture models is **checked into
the repo** and serves three purposes at once:

1. **Golden test** — CI regenerates (`gradle generateFixtures`) and fails on any byte diff, so
   every generator change shows its blast radius in review.
2. **Compile test** — the generated `BUILD.bazel`/`types.h` build as ordinary Bazel targets in
   `bazel test //...` on every platform, warning-clean.
3. **Behavior test** — hand-written GoogleTests (`examples/*/generated_types_test.cc`) pin the
   generated API's semantics: enum unknown-value preservation, union accessors, equality,
   optionality.

This is smithy-rs's `codegen-*-test` generate→compile→run pipeline with the golden check folded
in; Phases 3–5 extend it to clients, servers, and the integration harness.

## Regenerating

```sh
cd codegen && gradle generateFixtures   # rewrites examples/{weather,cafe}/generated
```

Determinism is a hard requirement (sorted includes, model-order members, stable map iteration);
`CppCodegenPluginTest.outputIsByteDeterministic` enforces it.
