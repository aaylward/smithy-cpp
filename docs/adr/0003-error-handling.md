# ADR-0003: Outcome-based error handling; no exceptions across the generated API

**Status:** Accepted (2026-07-06)

## Context

Generated C++ APIs need an error-reporting convention. C++ codebases are split between
exceptions and result types, and many large consumers build with `-fno-exceptions`.

## Decision

- Generated client operations and server handler methods return `smithy::Outcome<T, Error>`, an
  `std::expected`-like result type (polyfilled until C++23 is table stakes; the alias switches to
  `std::expected` when the floor rises).
- Modeled errors derive from `smithy::ModeledError`; transport and deserialization failures use
  distinct error categories, all carried in `Outcome::error()` with code, message, and
  retryability metadata, plus `ErrorsAs<T>()` accessors for typed access.
- Exceptions never cross the generated API boundary. The runtime must be buildable with
  `-fno-exceptions` (validated once conformance CI exists); internal use of exception-throwing
  std APIs is avoided in hot paths.

## Consequences

- Works for exception-free codebases; callers who prefer exceptions can trivially wrap.
- Server handlers report modeled errors by returning them, which keeps handler signatures
  symmetric with client operation signatures — important for the client↔server integration
  harness that reuses fixture handlers.
- C++20 baseline: gcc 11+ / clang 14+ / MSVC 19.30+ (confirmed here from PLAN §9 open question 3).
