# ADR-0020: Session detach/resume — a grace period on the fan-out registry

**Status:** Accepted (2026-07-20). Issue #116, the reconnect half of the
Golf-hub consumer assessment (issues #112/#113 → ADR-0017/0018).
Implemented: `SessionRegistry Options::{grace_period, on_expired,
queue_while_detached}`, `SessionRegistry::{Detach, Resume}`, the resume
handshake and redial patterns in the production guide, the `examples/chat`
async hub's resume flow.

## Context

ADR-0016 states the streams' base posture plainly: a dropped connection
loses in-flight state, and reconnect plus state recovery are entirely the
consumer's. For browser-served session apps — flaky mobile networks, page
reloads, laptop lids — reconnect is table stakes, and the deployed Go hub
this stack keeps being measured against shows both what consumers build
and how it goes wrong.

The Go hub's semantics: on disconnect, the player's session enters a
five-minute grace period — kept in room and game, marked disconnected,
cleanup timer armed. A token-authenticated reconnect within grace swaps
the new connection into the existing session and replays the current
authoritative state. Grace expiry removes the player and collects empty
rooms. Its defects are as instructive as its features: lacking an
identity-keyed swap primitive, it finds "the session that just
disconnected" by scanning recent-disconnect records with a one-second-age
heuristic — wrong under concurrent disconnects — and it burns a goroutine
per disconnect on the cleanup timer.

After ADR-0017/0018 the primitives half-exist here: tickets give pre-101
identity, and `Remove`-then-`Add` is the documented id-swap idiom. But
every consumer still hand-rolls the grace bookkeeping (deadline maps or a
thread per disconnect), the swap dance against racing expiry and racing
duplicate dials — exactly the code the Go hub got wrong — and the
exactly-once cleanup on expiry. The registry already owns the id→session
map and each session's delivery lifecycle; grace is a small, well-shaped
extension of what it already guards.

## Decision

**Two options, two methods, on `SessionRegistry`.**
`Options::grace_period` (zero keeps detach disabled and the registry
byte-identical to ADR-0017/0019 behavior) and `Options::on_expired`, plus:

- `Detach(id)` — the handler's abrupt-loss exit path. The entry stays in
  the map, marked detached with its deadline armed; its delivery stops
  (the writer thread exits and is reaped; an async chain goes idle) and
  its handle is dropped, so a detached entry is pure bookkeeping — **zero
  per-session threads**, structurally. False for an unknown id, an
  already-detached id, or when grace is disabled.
- `Resume(id, handle)` — **the identity-keyed atomic swap** the Go hub
  lacked. It succeeds only on a detached entry within grace: the new
  handle replaces the dropped one, delivery re-arms (writer or completion
  chain, per the new handle's `SupportsAsync`), and the pending expiry is
  cancelled — all under one lock hold, so the races that heuristic
  implementations lose (resume vs. expiry, two dials claiming the same
  id) are settled in one place, testably. False otherwise: on an attached
  entry (that id is live — the application decides whether to kick it),
  an expired or unknown id, or when grace is disabled.
- `Remove(id)` — unchanged: immediate removal for leave/kick, detached or
  not, and never runs `on_expired`.

**Expiry runs `on_expired(id)` exactly once, mutually exclusive with a
successful `Resume`.** This is the guarantee that is genuinely hard to
get right ad hoc and the reason cleanup belongs behind a callback instead
of in N hand-rolled timers. The callback runs with no registry locks held
(it may call back in — the slow-consumer precedent) after the entry has
already left the map; a concurrent `Resume` therefore either wins the
entry before expiry claims it or fails cleanly, never both.

**One lazy expiry thread per registry, not one per disconnect.** Created
on the first `Detach`, parked on a condition variable until the nearest
deadline, joined by the destructor. Per-session cost stays zero; the
registry pays one thread only once grace is actually in use.

**Events sent to a detached id are dropped by default** (`SendTo` and
`Broadcast` report them unqueued), because the blessed recovery model is
snapshot replay — the resume handler sends authoritative current state,
which supersedes anything missed. `Options::queue_while_detached` opts a
registry into retaining events in the existing bounded queue instead
(capacity and slow-consumer policy apply unchanged — with the close-on-full
default, policy on a full detached queue falls back to dropping the
event, since there is no live session to close); delivery resumes with the
swap. Either way, grace never turns a disconnect into unbounded retention.

**`Drain` expires detached entries immediately** — `CloseAll` closes the
live sessions, and a draining server is not waiting five minutes for
ghosts. Each immediate expiry runs `on_expired` (the cleanup it promised),
still exactly once. The destructor does the same.

**The resume handshake is documentation, not machinery** (issue #116
proposal 2). ADR-0018's ticket pattern extends to a resume ticket: the
same authenticated unary mint, bound to the session id, carried on the
reconnect upgrade's `@httpQuery` member, validated in the gate before any
101. The handler calls `Resume` instead of `Add` and sends the snapshot as
its first events. The production guide now walks the whole loop — mint →
gate → `Resume` → snapshot replay — and states the posture out loud:
**recovery is snapshot replay, not message replay**; ADR-0016's "in-flight
state is lost" stays true across reconnects. Client-side redial (backoff,
re-mint, re-dial, resume handshake) is likewise documented as a worked
pattern rather than automated: it is application protocol at both ends,
and a `RedialPolicy` knob on generated clients waits for a consumer with a
concrete shape.

## Non-goals

Message-level resume — sequence numbers, acks, replay-from-cursor,
exactly-once — is a different, much larger feature; snapshot-on-resume
covers the session-oriented applications this stack serves. Cross-process
or cross-host migration: `SessionRegistry` is single-process by scope
(ADR-0017 non-goals) and grace inherits that. Surviving server restarts: a
restarted process has no sessions to resume into; that is application
persistence (and mirrors the deployed hub, whose restart invalidates all
tokens by design). The reserved close-code accessor (ADR-0015) that would
let a server distinguish "resume soon" from "session expired" at close
stays future work; this ADR only notes the connection.

## Alternatives rejected

- **A thread (or timer object) per detached session** — the Go hub's
  goroutine-per-disconnect, transplanted. Rejected: grace exists for flaky
  fleets, where disconnects are the common case; per-disconnect threads
  make the failure mode (mass disconnect) the expensive case. One parked
  registry thread bounds the cost.
- **Lazy expiry only (sweep on registry activity, no thread)** — free, but
  `on_expired` is a cleanup contract (collect the game, tell the room),
  and cleanup that waits for unrelated traffic is not timely on a quiet
  server. The hybrid (sweep opportunistically, thread as backstop) adds
  two code paths for one guarantee.
- **Detach as a flavor of Remove plus an application-side deadline map** —
  the status quo this ADR replaces. The registry cannot then guarantee
  zero-thread detach or the expiry/resume exclusion; every consumer
  rebuilds the swap dance, which is the bug class the issue documents.
- **Unbounded (or default-on) retention while detached** — turns every
  grace period into a memory liability sized by broadcast rate, to buy
  replay the snapshot model does not need. Retention is opt-in and
  bounded by the queue capacity that already exists.
- **Automatic client redial now** — automates the least error-prone step
  of a loop whose hard parts (re-mint, resume handshake, snapshot
  handling) are application protocol. Documentation first; a knob later if
  a consumer brings a concrete shape.

## Consequences

A hub's reconnect story shrinks to: `Detach` on abrupt loss, a
gate-validated resume ticket, `Resume` + snapshot in the handler, and an
`on_expired` that does what the hand-rolled timer used to — with the
registry owning the races. The registry gains one optional thread and a
detached state whose interactions (with `Drain`, `Remove`, both delivery
modes, and the queue policy) are pinned by tests rather than rediscovered
per consumer. The chat hub demonstrates the loop end to end, driven as
real processes; applications needing stronger delivery than snapshot
replay own it at the protocol level, unchanged.
