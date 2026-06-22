---
name: Design spec
about: Authoritative design-session output that drives implementation (the test oracle)
title: "spec: <feature>"
labels: spec
---

<!--
This is the AUTHORITATIVE design for a feature — it outranks a casual bug
report's "fix sketch". An implementer derives tests from the Acceptance
criteria below and treats the Decision & rationale as binding.

If a section does not apply, say so explicitly rather than deleting it.
-->

## Context

<!-- The problem, and where it sits in the system. Link the issue(s) this
came from. -->

## Decision & rationale — the "why" (binding)

<!-- What we decided, and WHY each part exists. The rationale is what makes a
test meaningful: e.g. "on_load_state carries `reason` so a cart can rebuild
stale derived caches per restore type and play a load sound only on
player-visible loads". -->

## Contract / guarantees (binding)

<!-- The observable behaviour the implementation must deliver, per surface /
platform. For cross-platform contracts, state that every leg
(native / WASM / libretro, host + guest) must behave identically — determinism
is the core contract. -->

## Acceptance criteria — the test oracle (binding)

<!-- A checklist. EACH item must become a test that *consumes* the behaviour
(reads the value, distinguishes the cases) — never just asserts that a callback
fired. Cross-platform items should assert identical output across legs
(`run_cart_all_legs*`). -->

- [ ]
- [ ]
- [ ]

## Non-binding notes (fix sketches, hunches)

<!-- Explicitly NOT the spec. Hints about likely implementation, places to look,
guesses — any of which may be wrong. Kept separate so they can never again
masquerade as the contract. -->

## Out of scope

<!-- What this deliberately does not cover, and why. -->

## Related ADRs

<!-- Cite governing ADRs (e.g. ADR-0087). If this settles something durable and
no ADR exists, note "needs an ADR" and link it once written — ../blyt-planning. -->
