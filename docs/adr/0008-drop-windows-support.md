# ADR-0008: Drop Windows support

**Status:** Accepted (2026-07-08)

## Context

Windows (MSVC) was in the CI matrix from Phase 0, on the theory that winsock and MSVC
conformance quirks are cheaper to carry continuously than to retrofit (PLAN §risk table).
Carrying it has meant: a winsock/`#ifdef _WIN32` fabric through the socket transport
(WSAStartup lifecycle, `SOCKET` vs `int` handles, `DWORD` timeouts, `ws2_32.lib` linkopts),
platform `select()`s in the Bazel rules, a `/std:c++20` MSVC config, Windows carve-outs in
tests (`json_conformance_test`, the hostile-framing suite, the timestamp differential's
reduced epoch range), and two Windows CI jobs per PR — while no known consumer targets
Windows and the production transport story (Beast + TLS, ADR-0006/0007) is aimed at
POSIX deployments. The tax was paid on every change to the transport layer; the #48
hardening series (HTTP/1.1 parser extraction, sanitizer matrix) kept running into it.

## Decision

Windows is not a supported platform. Linux and macOS are the supported matrix, both under
asan+ubsan.

- The socket transport is POSIX-only: no winsock includes, no `WSAStartup`, `int` fds,
  `timeval` timeouts, unconditional `SO_REUSEADDR` on the listener.
- Bazel: no `@platforms//os:windows` branches (`COPTS`, `ws2_32.lib`, `_WIN32_WINNT`,
  `target_compatible_with` carve-outs) and no `build:windows` config.
- CI: the `windows-msvc` bazel job and the `windows-2022` consumer job are removed.
- Tests run their full scope everywhere: the hostile-framing suite unconditionally, the
  timestamp differential over its full pre-1970 epoch range.

## Consequences

- Two fewer CI jobs per PR; the transport layer sheds its largest `#ifdef` surface, and
  future transport work (e.g. Beast fuzzing) doesn't carry a Windows variant.
- A future consumer wanting Windows re-opens this decision with a concrete use case; the
  history (pre-removal code) is in git, but re-adding is a real port, not a revert.
- The `SO_NOSIGPIPE`/`MSG_NOSIGNAL` conditionals remain: those are Linux-vs-macOS/BSD
  differences, not Windows ones.
- Supersedes the Windows rows of PLAN §CI-matrix and the Linux/macOS/Windows claims in
  ADR-0004/ADR-0005 (historical documents, left unedited).
