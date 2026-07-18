# ADR-0012: Client-address derivation is a pure helper anchored at the L4 peer

**Status:** Accepted (2026-07-18)

## Context

ADR-0010 made `HttpRequest.peer_address` a transport-stamped fact: the TCP
connection's remote endpoint, never read from the wire. That is the right
identity for a directly reachable service and the wrong one behind a reverse
proxy or load balancer, where every request's peer is the proxy. The
conventional bridge is `x-forwarded-for` — each proxy appends the address it
accepted the connection from — but the header starts life client-authored:
whatever the client wrote arrives ahead of anything a proxy appended. A
policy that reads it raw (as docs/production-guide.md's `Guard` example did)
hands the attacker its key: a fresh spoofed entry per request evades a rate
limiter, a victim's address exhausts the victim's bucket, and an allowlist
keyed on it is a bypass, not a control.

The correct derivation is the one nginx's `real_ip` module
(`set_real_ip_from` + `real_ip_recursive on`) and Envoy's trusted-hops
implement: only entries appended by proxies you trust count, and trust is
evaluated starting from the one non-forgeable fact — the L4 peer.

Placement options considered:

1. **Rewrite `peer_address` in the transports** (an Options knob): rejected.
   It destroys the L4 truth ADR-0010 defined the field to carry, changes the
   field's meaning based on config, and needs a copy per transport.
2. **Derive in `InvokeHandlerGuarded`** (where ADR-0011 mints trace
   identity): rejected. The guard is config-free and stamps framework facts;
   which proxies to trust is deployment policy, and threading policy config
   through every transport's guard call is the wrong altitude.
3. **Leave it to applications**: rejected. The walk is short but
   security-sensitive, and the obvious naive versions (read it raw, take the
   leftmost entry) are the vulnerable ones — our own documented example
   included.
4. **A pure runtime helper the policy layer calls explicitly**: chosen.

## Decision

`smithy/http/forwarded.h` ships two pieces, both pure and SDK-free:

- **`TrustedProxies`** — the deployment's trust boundary as a CIDR set
  (`{"10.0.0.0/8", "2600:1f00::/24"}`; a bare address is a host route).
  Parsed at construction; a malformed entry throws `std::invalid_argument`,
  because a misconfigured trust boundary must fail deployment, not silently
  widen or narrow. A CIDR set rather than Envoy-style hop counts: hop counts
  are only correct when every request path traverses exactly that many
  proxies, and the failure mode of a wrong count is silent forgeability.
  The default-constructed set trusts nothing.
- **`ClientAddress(request, trusted)`** — the rightmost-untrusted walk,
  anchored at `peer_address`: if the peer is not a trusted proxy the peer
  *is* the client and the header is ignored wholly (it is client-authored
  noise); otherwise walk the `x-forwarded-for` entries right to left —
  rightmost were appended last, by proxies closest to us — skipping trusted
  entries; the first untrusted entry is the client. Spoofed prefix entries
  are unreachable: the walk stops at the entry the outermost trusted proxy
  appended, which is the real client as the edge saw it.

Pinned semantics:

- Returns the bare numeric address in canonical (`inet_ntop`) form — no
  port, no brackets — so it is directly usable as a policy or metrics key.
  IPv4-mapped IPv6 (`::ffff:203.0.113.9`, what a dual-stack listener
  reports) is treated as the embedded IPv4 everywhere: as a candidate, in
  entries, and in trust matching.
- A malformed entry stops the walk at the last vetted position (the nearest
  trusted hop): garbage is never trusted and never reached past.
- A chain exhausted with every entry trusted yields the leftmost entry —
  the request originated inside the trusted tier itself.
- Entries tolerate the forms real proxies emit: bare IPv4/IPv6, `ip:port`,
  `[v6]`, `[v6]:port`; multiple `x-forwarded-for` headers join in order per
  RFC 9110 list semantics.
- An empty or unreportable `peer_address` (Loopback, direct handler-driven
  tests) derives an empty client — nothing is known, and empty never
  matches a trust set.

`peer_address` itself stays untouched everywhere: the transport-stamped fact
remains the L4 truth, and derivation is an explicit, configured act at the
layer that owns policy (`Guard` admit callbacks, observability sinks,
handlers via `context.request`).

## Consequences

- docs/production-guide.md's `Guard` example keys its limiter on
  `ClientAddress` instead of the raw header, and the empty-key
  probe-starvation hazard the old example documented dissolves: with no
  spoofable key, health probes and direct-connect clients each rate-limit as
  their real peer.
- `x-forwarded-for` only. RFC 7239 `Forwarded` support would be a
  compatible extension (same walk, one more entry parser), adopted when a
  real proxy in front of a consumer emits it.
- The helper is the key extractor, not the policy: a per-client rate
  limiter composing `TrustedProxies` + `ClientAddress` + `Guard` +
  `TooManyRequests` is the natural follow-on, and stays an application
  choice per the middleware contract.
- Servers that never sit behind a proxy simply never construct a
  `TrustedProxies`; nothing changes for them.
