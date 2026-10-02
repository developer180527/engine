---
status: plan
id: WO-051
title: A kit-ABI component that changes meaning refuses old kits, instead of loading them
program: providers
priority: P1
size: S
state: todo
contracts: [kit-abi]
touches:
  - include/engine/game_module.h
  - src/components/collision_events.h
source: review 2026-10-03 of WO-048 (b39bbed)
---
## Why
The kit gate hashes a shared component's LAYOUT (sizeof, alignof), so a change of MEANING with the same layout loads old kits silently.

WO-048 did exactly that to `CollisionEvents`. A component that used to exist only on ticks with a contact now stays on every body that has touched anything, with empty lists on quiet ticks. A kit system written as "for each CollisionEvents, play the impact sound" now fires every tick for every such body. It loads, links and passes the gate.

## Done when
- [ ] every component in the kit ABI declares `static constexpr uint32_t kAbiRevision`, and `ENGINE_ABI_HASH_TYPE` folds it into the hash
- [ ] a kit built against an older revision is refused at load, and the message names the component and its migration guide in `docs/guides/`
- [ ] a golden-file test records each kit-ABI component header with its revision: editing such a header fails until the change is recorded as "revision bumped" or "comment only", so a meaning change cannot pass as an ordinary edit
- [ ] `CollisionEvents` is bumped for WO-048's change, and `docs/guides/` gains the migration note (empty lists mean "no contact this tick")
- [ ] `collision_events.h`'s top comment ("Removed when no events remain") is corrected; it contradicts the rule written below it
- [ ] mutation: a header edit with no golden update fails the test; a kit built at the old revision is refused

## Contract
Adds to `kit-abi`'s **Errors**: a kit built against an older MEANING of a shared component is refused by name, as an older layout already is.

Nothing: a component with no `kAbiRevision` is revision 0, so a kit built before this order loads only if no component it uses has been bumped since.

## Not in scope
The `CollisionEvents` redesign itself (WO-052).
