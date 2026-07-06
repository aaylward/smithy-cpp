# ADR-0001: Write the generator on the JVM with Smithy's DirectedCodegen

**Status:** Accepted (2026-07-06)

## Context

We need a code generator that consumes Smithy models and emits C++ clients and servers. Two
implementation strategies were considered:

1. A JVM (Java 17) generator built on the official
   `software.amazon.smithy:smithy-codegen-core` `DirectedCodegen` framework, packaged as a
   `smithy-build` plugin — the approach used by every mature Smithy generator
   (smithy-rs, smithy-typescript, smithy-go, smithy-swift).
2. A standalone generator (C++ or Python) consuming the JSON AST emitted by `smithy build`.

## Decision

Option 1. The framework gives us model validation, trait indexes (`HttpBindingIndex`,
`OperationIndex`), symbol/dependency management, topological shape ordering, protocol-test trait
plumbing, and forward compatibility as the Smithy IDL evolves. smithy-rs — our reference
implementation (PLAN §3.2a) — proves the approach for a systems language with both client and
server generation.

## Consequences

- The JVM is a **build-time-only** dependency. Consumers never need Java at runtime, and the
  Bazel rules (Phase 6) run the generator hermetically via a `rules_java` toolchain, so users
  only ever type a Bazel target or CLI command.
- Contributors to the generator write Java; contributors to the runtime write C++. This split is
  identical to smithy-rs (Kotlin/Rust) and has not been a barrier there.
- The generator is published to Maven Central and consumed by the Bazel toolchain by version pin.
