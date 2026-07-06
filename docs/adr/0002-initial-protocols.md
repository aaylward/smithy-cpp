# ADR-0002: Ship restJson1 and rpcv2Cbor from the start

**Status:** Accepted (2026-07-06)

## Context

Smithy protocols are pluggable. We need to choose which protocols the first generator versions
implement, for both clients and servers. A single initial protocol risks baking that protocol's
assumptions into the "abstraction"; deferring RPC support was rejected during plan review.

## Decision

Implement **two protocols in the same phases** (PLAN Phases 3–4):

- **`aws.protocols#restJson1`** — REST: full HTTP binding surface (labels, query params,
  headers, payloads, status codes). It lives in the `aws.protocols` namespace for historical
  reasons but is the de-facto standard protocol for generic Smithy REST services and carries no
  AWS coupling beyond protocol-mandated names.
- **`smithy.protocols#rpcv2Cbor`** — RPC: the vendor-neutral protocol in the core Smithy
  namespace (`POST /service/{Service}/operation/{Operation}`, CBOR bodies).

Different bindings *and* different wire formats (JSON vs CBOR) force the protocol-generator and
serde abstractions to be genuinely format-agnostic from day one. Both protocols have official
conformance suites (`smithy-aws-protocol-tests`, `smithy-protocol-tests`) that we generate tests
from.

## Consequences

- The runtime carries both a JSON and a CBOR serde module behind one reader/writer interface
  (Phase 1).
- Further protocols (e.g. JSON-RPC 2.0, restXml) slot in behind the same `ProtocolGenerator`
  interface later, added on demand.
- smithy-cpp stays vendor-neutral: no AWS traits, auth, endpoint logic, or SDK behaviors (PLAN §2).
