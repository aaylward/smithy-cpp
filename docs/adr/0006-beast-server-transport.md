# ADR-0006: Boost.Beast is the production server transport

**Status:** Accepted (2026-07-06). Amends ADR-0005. Implemented: `//runtime:http_beast` (`BeastServerTransport`).

## Context

ADR-0005 shipped Phase 1 with a dependency-free, blocking, one-connection-at-a-time,
plaintext-only HTTP/1.1 server. That was the right way to unblock the Phase 1 exit criterion,
but it is not what generated services should run on: no TLS, no concurrency, no backpressure.
Plan review asked: which real embedded server dependency do we standardize on so the generated
servers are good by default?

## Options considered

Hard constraints: (a) license compatible with Apache-2.0 redistribution, (b) available in the
**Bazel Central Registry** — our own module must be publishable to the BCR (Phase 6), and BCR
modules may only depend on other BCR modules, (c) a credible path to Phase 8 (WebSockets,
event streams).

| Candidate | License | In BCR? | Notes |
|---|---|---|---|
| mongoose | GPLv2 / commercial | no | License disqualifies it outright |
| civetweb | MIT | **no** (probed) | Solid embedded C server + TLS + WebSocket, but we would have to package and maintain a BCR module for it |
| cpp-httplib | MIT | **no** (probed) | Simple, but blocking-only and no WebSocket — weak Phase 8 story |
| uWebSockets | Apache-2.0 | no | Fast, WebSocket-native, but unusual build and no BCR presence |
| **Boost.Beast (+ asio)** | BSL-1.0 | **yes** (modular `boost.beast` verified) | C++-native HTTP + WebSocket on asio; full control over the server loop; TLS via asio SSL (`boringssl` is in BCR) |

## Decision

**Boost.Beast, via the modular `boost.beast` BCR module**, is the supported server transport:

- The runtime gains a `BeastServerTransport` implementing the existing `HttpServerTransport`
  interface: asio thread pool, per-connection timeouts, keep-alive, graceful shutdown — the
  Phase 7 robustness knobs get a real substrate.
- Phase 8 bidirectional streaming uses `beast::websocket` on the same dependency, and asio is
  the natural base for the eventual coroutine async API.
- TLS lands as asio SSL on `boringssl` (BCR) when the client story needs it too; **libcurl**
  (also in BCR) remains the planned production *client* transport.
- The ADR-0005 built-in socket transport is **demoted to a test/reference transport**: it keeps
  zero-dependency unit tests fast and hermetic, and documents the interface contract. It is not
  the default for generated servers once Beast lands.
- Implementation is scheduled as its own PR immediately after Phase 2, so Phase 4's generated
  servers and Phase 5's integration harness bind to Beast from the start.

## Consequences

- New bazel_deps: `boost.beast` (pulling modular `boost.asio` etc.). Build-time cost is bounded
  to the runtime `//runtime:server_beast` target; consumers who only need the client or loopback
  don't pay for it.
- PLAN §9 open question 4 ("Beast-only vs plain-asio minimal impl") is resolved: Beast.
- The generated-server quality bar ("easy, good by default") is owned by the runtime transport,
  not by generated code — generated bindings stay transport-agnostic behind
  `HttpServerTransport`.
