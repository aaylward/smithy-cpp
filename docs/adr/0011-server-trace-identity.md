# ADR-0011: The server mints the request's trace identity at ingress

**Status:** Accepted (2026-07-18)

## Context

Until now the server treated trace identity as strictly the client's business:
`GenerateTraceContext()` was exercised only by the client's `PropagateTraceContext`
interceptor, `Observe` reported the inbound `traceparent` verbatim and left it empty when
absent, and the only server-minted id was #41's exception-path correlation uuid. Issue #46
tracks the gap: a request from a client that sends no `traceparent` — most of them — is
uncorrelatable across `Observe`, handler logs, and the contained-exception 500, and #91's
span work has no per-request identity to anchor to.

ADR-0010 gave handlers the raw request, so whatever identity exists is already
handler-visible; what was missing is the guarantee that one exists.

Placement options considered:

1. **In `InvokeHandlerGuarded`** — the guard every transport (Beast, socket, Loopback, and
   any consumer-written `HttpServerTransport`) already calls once per request, before the
   middleware chain runs.
2. **Per transport** — three copies of the same stamp, and middleware composed outside a
   transport that forgot one would silently regress.
3. **In `Router::Route`** — too deep: `Observe` and other middleware run outside the router
   and would still see the empty header.

Representation options: a new annotation field on `HttpRequest`, versus writing the minted
identity into the request's `traceparent` header itself.

## Decision

Mint in `InvokeHandlerGuarded` (option 1), into the header itself:

- A valid inbound `traceparent` continues verbatim — the caller's trace is joined, and the
  caller's span id is preserved for `Observe`.
- An absent, malformed, or duplicated one (W3C counts multiple `traceparent` headers as
  malformed) is replaced with a fresh root context (`GenerateTraceContext()`), per W3C
  trace-context's restart-the-trace rule; a restart also drops any inbound `tracestate`,
  since vendor state from the abandoned trace must not pair with the fresh root.
- The header, not a parallel field, carries the identity: every existing consumer —
  `Observe`'s header read, handlers via `context.request`, future request forwarding — sees
  one consistent story with zero new API surface, exactly as if a conforming client had
  called. (This deliberately trades "the request records only what the peer sent" for one
  source of truth; the peer_address-style annotation field remains the pattern for facts
  that have no wire representation.)
- The contained-exception 500's `x-correlation-id` becomes the request's trace id — one
  identity across the client-visible body, the `std::clog` line, and the distributed trace.
  The #41 uuid mint is gone: the guard mints before every handler call, so the
  parse-failure branch is unreachable and defensively yields an empty id.

`InvokeHandlerGuarded` takes the request by value to own the stamp; transports move their
requests in.

## Consequences

- `RequestObservation::trace_parent` always parses for transport-served requests; metrics
  and logs can key on trace id unconditionally. Handler chains driven directly in tests
  (`server.Handler()(request)`) are unminted by design — minting is a transport-ingress
  concern.
- The remaining #46 correlation item (returned-error 500s carry no id) reduces to stamping
  the same trace id in the generated error path.
- Servers do not rewrite `parent_id` for downstream propagation — opening child spans stays
  the handler's/adapter's job (#91), with `GenerateSpanId()` as the building block.
