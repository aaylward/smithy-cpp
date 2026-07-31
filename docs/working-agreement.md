# Working agreement

How work gets picked up, built, reviewed, and shipped in this repo. Written
down so a future session starts where the last one left off instead of
rediscovering the same conventions.

This is process, not architecture. Architecture lives in `docs/adr/`.

## Shipping a change

**One item, one PR.** Work items come off a tracking issue (e.g. #109, the
Core Guidelines conformance review). Take the highest-severity open item,
finish it, ship it, then take the next. Don't batch unrelated fixes.

**Altitude review first.** Before writing any code: read the cited code,
confirm the finding is actually real (several tracked items turned out to be
sharper *or* narrower than their description), and propose a plan. Only then
implement. Jumping straight to a fix hides the cases where the reported issue
is a symptom of something bigger.

**Ask about scope when the sizes differ materially.** If the plan has a
minimal version and a thorough version that lead to genuinely different work,
ask — with a recommendation, not a survey. If they only differ cosmetically,
pick the obvious one and say so.

**Don't open a PR unless asked.** Commit and push when the work is done; open
the PR only on request. Reference the tracking issue and, when the issue is a
checklist, tick the item once merged.

**Update the tracking issue.** Fold new data (reproductions, measurements,
scope corrections) back into the issue so it stays the source of truth.

## Review panel

Before committing anything non-trivial, run a self-review panel:

- **Three independent agents, three distinct lenses.** Typically correctness
  and control flow; concurrency, threading, and resource safety; and
  tests/docs/CI-gates. The lenses should barely overlap.
- **Each agent hunts, then tries to refute its own findings** before
  reporting. This is what keeps the signal-to-noise usable.
- **Verify the survivors yourself** before acting on them. Agents are
  sometimes confidently wrong; don't take a finding at face value.

The panel has earned its cost — it caught a real defect on several
consecutive PRs (an INT64_MIN decompose UB, a keep-alive framing gap, an
unguarded WebSocket upgrade target, and two libraries missing from a new CI
gate).

**If the panel didn't run, say so.** A restart or an interrupt can kill it.
Report that plainly rather than letting the reader assume the step happened.

**Answer review questions with tests, not paragraphs.** See below — this is
the single highest-leverage rule in this document.

**Look for simplification on every review.** See the next section: a review
that only hunts for defects is doing half the job.

## Design and simplification

**The goal is simple, readable code with clear interfaces.** Not clever code,
not maximally general code — code the next reader understands without a tour.
An interface that takes a paragraph to explain is a design problem wearing a
documentation problem's clothes; fix the interface.

**Testability is a core requirement, not a side effect.** If something is hard
to test, that is a design defect, and the design is what changes — never
settle for testing it badly, testing it indirectly, or not at all. The seams
that let a test drive the behavior (an injectable clock, a transport
interface, a callable policy) are part of the deliverable, not scaffolding
bolted on afterward.

**Every review is a simplification opportunity.** Alongside hunting defects,
ask: does this abstraction earn its keep? Can two near-identical paths become
one? Is this special case actually special? Can this be deleted outright? The
best review outcome is often less code, not more.

**Characterize before you refactor — positive *and* negative tests, first.**
Before changing the shape of existing code, cover it with tests that pin both
what it does and what it *refuses* to do, and confirm they pass against the
unchanged code. Only then refactor.

The ordering is the whole point. Tests written afterward describe the new
code's behavior, not the behavior you meant to preserve — that is how a
refactor silently becomes a rewrite. And the negative half is not optional:
positive tests alone let a refactor quietly *widen* behavior, accepting input
the original rejected, which is exactly the shape of a security regression.
Passing tests before and after are what make the change a refactor rather
than a hope.

## Testing bar

**A test beats an argument.** If a behavior is interesting enough to question,
debate, or reason carefully about — in a review, in a PR thread, or in your
own head — write a test that runs in CI instead. This is *always* better than
reasoning about correctness. Reasoning is invisible to the next reader, decays
as the code moves underneath it, and is exactly what the person who wrote the
bug already did. A test is executable, survives refactors, and fails at the
moment the property breaks rather than the moment someone notices.

In practice: when a reviewer asks "what happens if X?", the deliverable is a
CI test named after X — not a reply explaining why X is fine. When you catch
yourself constructing an argument for why something must be correct, stop and
write the test that would prove it.

**Comments are not a contract. CI tests are.** A doc comment stating a rule
constrains nothing. It is intent — and intent that nothing enforces drifts
from the code the moment someone edits without reading it, silently, with no
failure anywhere. "Comment as contract" is not an acceptable design; if a
property matters, something must *fail* when it is violated: a test, a type,
or a fail-fast check that a test then pins.

This repo has the receipts, and every one of them is an open item on the
conformance issue:

- `interceptor.h` documents that hooks "must not throw" — the virtuals are
  not `noexcept` and `retry.cc` calls them bare, so a throwing hook
  propagates straight out of a generated client operation.
- `timestamp.h` promises parsing with "no locale" — `ParseEpochSeconds`
  converts with `std::strtod`, which honors `LC_NUMERIC` and silently drops
  fractional milliseconds under a comma-decimal locale.
- `middleware.h` states "neither callback may be null" for `Guard` — nulls
  are accepted and every request 500s on `std::bad_function_call`.

Each one was true prose and false behavior for as long as nobody tested it.
Keep writing comments — they carry the *why*, which no test can — but the
comment documents the contract; it is never the contract itself.

**The Beyoncé Rule: if you liked it, you should have put a test on it.** Every
observable behavior worth keeping gets a test. Apply this liberally and at
every level that fits the behavior:

- **unit** — the mechanism itself,
- **integration** — the behavior through the real wire, transport, or codec,
- **out-of-tree consumer example** — where the behavior is part of the
  contract a consumer depends on, prove it through the module boundary the
  way a consumer would actually hit it.

An untested observable behavior is not a guarantee; it is a coincidence that
currently holds.

**TDD for bug fixes.** Write the failing test first, watch it fail for the
right reason, then fix it.

**Consumer and e2e tests that flex the feature, not smoke tests.** New
functionality needs a test in the out-of-tree consumer module
(`examples/bazel-consumer`) that actually demonstrates it working through the
module boundary — the way a real consumer would use it.

**Fuzz targets for anything that parses.** Decoders, framing, URIs, headers,
compression. See `docs/fuzzing.md`.

**Mutation-test negative and security tests.** A test asserting that
something is *rejected* must be proven to fail when the property it pins is
broken — temporarily remove the check, confirm that exact test fails with its
own message, then restore. A test that passes for the wrong reason is worse
than no test, because it advertises coverage that isn't there. This is how
the client TLS hostname and version-floor tests were validated.

**Prove isolation with a control.** When a negative test asserts a failure,
add the positive twin that shares the fixture (e.g. the same hand-built
listener, one version higher) so a broken fixture can't masquerade as the
property holding.

**Re-run timing-sensitive tests.** Anything with threads or sockets gets
`--runs_per_test=15` or so before it's trusted.

## Verification before pushing

Run these, and don't report success on a step that didn't run:

- `clang-format` on every changed `.h`/`.cc`
- `clang-tidy` on changed `.cc` — this is a **separate CI job** from the
  Makefile's `lint` target, and it has failed PRs that were otherwise clean
  (e.g. `readability-use-anyofallof` on hand-rolled scan loops)
- `buildifier` for changed BUILD files
- the full runtime suite
- sanitizers: asan/ubsan, plus tsan for anything touching concurrency
- `make noexcept` — the ADR-0003 `-fno-exceptions` gate
- the consumer module where it's reachable

**Be explicit about what couldn't be verified locally, and why.** The sandbox
has a pre-existing `rules_android` resolution failure in `//codegen`'s JVM
plugin that blocks consumer targets needing generated code. When you hit a
limitation like that, *prove it's pre-existing* by reproducing it with your
changes stashed, then say so in the PR body. CI runs those jobs natively.

## Docs and changelog

- Update docs in the same PR as the code: ADRs, guides, public header
  contract comments.
- Add a CHANGELOG entry.
- **If a change alters an ADR's stated posture, amend the ADR.** Leaving an
  ADR contradicting the code is a defect in its own right — ADR-0003 was
  amended when contract violations moved from `throw` to fail-fast, and again
  when recoverable config moved to an `Outcome`.
- Keep the claims accurate. Don't write that something is covered "everywhere"
  when a subtree is deliberately excluded; name the exclusion.

## Dependencies and infrastructure

**Re-check assumed limitations instead of repeating them.** A limitation
recorded in a previous session may no longer hold — the Bazel sandbox
restriction was re-examined and turned out to be workable via git-based module
overrides.

**When you find a workaround, make it reusable.** Document it in the repo docs
and add a setup script so the next session gets it for free
(`bazel/make-git-overrides.sh`, wired to a SessionStart hook).

**Dependency bumps: don't trust the PR's own green CI.** Check how stale its
base is. A renovate PR whose checks passed against a base 50 commits old has
validated a tree that no longer exists. Merge it into current `main` locally
and run the affected suites — that's the only signal that matters. For a
security-sensitive dependency, also ask what the existing tests actually
*assert* (a posture test that checks the negotiated cipher is worth far more
than one that checks the connection succeeded).

## Communication

- Raise a concern in a sentence or two, then proceed with the work. Don't
  stop and wait unless proceeding would be unsafe or wasted.
- Report outcomes faithfully: if a step was skipped, say it; if tests failed,
  show it.
- Distinguish real defects from nits when reviewing, and say which is which.
- Don't re-litigate decisions already made.

## Operational notes

- **Merge commits on the working branch.** After a PR merges, the branch is
  reset onto `main`, which brings GitHub's own merge commit (committer
  `noreply@github.com`) along. Tooling may flag it as unverified; it is
  already-published upstream history and must not be amended.
- **Wedged PRs.** The owner sometimes merges locally and pushes `main`
  directly, leaving the PR object open with phantom conflicts. Before
  believing a conflict, check whether the PR head is already an ancestor of
  `origin/main`; a push to the branch un-wedges it.
