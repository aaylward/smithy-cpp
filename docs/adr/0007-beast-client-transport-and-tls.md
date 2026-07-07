# ADR-0007: Boost.Beast client transport with TLS (and server TLS termination)

**Status:** Accepted (2026-07-07). Extends ADR-0006. Implemented: `BeastHttpClient` and
`BeastServerTransport` TLS options in `//runtime:http_beast`.

## Context

ADR-0005/0006 left the client side of the transport story unfinished: the built-in
`SocketHttpClient` is connection-per-request, plaintext-only (`https://` endpoints were
rejected outright), and the "pooling in the curl transport" Phase 7 item died with the curl
plan. Generated clients had no production-grade wire, and Phase 8 (WebSockets) needs a
Beast-side client foundation anyway.

## Decision

1. **`BeastHttpClient`** in `//runtime:http_beast`, next to the server transport (same Boost
   dependency set, one Beast implementation TU): synchronous `Send()` built on Beast's
   *asynchronous* operations driven to completion per call — Beast stream timeouts only apply
   to async operations, so this is what gives blocking sends a real `request_timeout_ms`.
   Keep-alive connections are reused through a small mutex-guarded idle pool
   (`max_idle_connections`); each connection owns its `io_context`, so concurrent `Send()`
   calls proceed independently. A pooled connection that died between requests (write failure
   or immediate EOF) is redialed once transparently.
2. **TLS via asio SSL over BoringSSL.** The BCR `boost.asio` module only compiles its SSL
   implementation behind a build flag every consumer would have to set, so instead
   `beast_src.cc` (already the exactly-one-TU home of Beast's implementation) includes
   `<boost/asio/ssl/impl/src.hpp>` and `//runtime:http_beast` depends on
   `@boringssl//:ssl` directly — the target is self-contained, no flags. Certificate *and*
   hostname verification are on by default;
   `ca_pem` swaps in a private CA (tests use a self-signed localhost certificate), and
   `verify_peer = false` exists as an explicit, documented footgun.
3. **Server TLS termination**: `BeastServerTransport::Options` gains
   `tls_certificate_chain_pem` / `tls_private_key_pem` (PEM text, not paths). The session
   loop is stream-type generic (`tcp_stream` vs `ssl::stream<tcp_stream>`); TLS sessions
   handshake before the read loop.
4. **`SocketHttpClient` stays the zero-dependency default** for `config.endpoint`; the
   dep-light rule (ADR-0005) holds — generated clients do not link Boost. `ParseEndpoint`
   now accepts `https://` (port 443 default, `Endpoint::tls()`), and generated `Create()`
   rejects an https endpoint without an injected transport, pointing at
   `BeastHttpClient::FromEndpoint`.

## Consequences

- The production stance is now symmetrical: Beast on both sides, TLS both directions,
  documented in the production guide; the loopback/socket transports remain for tests and
  simple plaintext deployments.
- BoringSSL enters the dependency graph only behind `//runtime:http_beast` + the asio `ssl`
  flag; `//runtime:client`, `:core`, and generated code stay Boost- and TLS-free.
- HTTP-level retry safety: the transparent redial only happens when the request bytes could
  not have been processed (write failed, or EOF before any response bytes); everything else
  surfaces as a retryable-per-policy transport error to the existing retry layer.
- Proxy support (CONNECT), client certificates (mTLS), and connection health checks are
  explicitly out of scope for 0.1.0; the Options struct leaves room for them.
