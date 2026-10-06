---
status: plan
id: WO-051
title: A kit-ABI component that changes meaning refuses old kits, instead of loading them
program: providers
priority: P1
size: S
state: done
done: 2026-10-06
evidence: tests/module_abi/tests/gate.rs (old_revision fixture refused, named); ctest kit_abi_headers; mutations in the log
contracts: [kit-abi]
touches:
  - include/engine/game_module.h
  - src/components/collision_events.h
  - src/runtime/module_loader.h
  - scripts/kit_abi_headers.py
  - tests/fixtures/kit_abi_headers.txt
  - tests/fixtures/abi_gate_module.cpp
  - tests/module_abi/tests/gate.rs
  - docs/guides/kit-abi-revisions.md
source: review 2026-10-03 of WO-048 (b39bbed)
---
## Why
The kit gate hashes a shared component's LAYOUT (sizeof, alignof), so a change of MEANING with the same layout loads old kits silently.

WO-048 did exactly that to `CollisionEvents`. A component that used to exist only on ticks with a contact now stays on every body that has touched anything, with empty lists on quiet ticks. A kit system written as "for each CollisionEvents, play the impact sound" now fires every tick for every such body. It loads, links and passes the gate.

## Done when
- [x] every component in the kit ABI declares `static constexpr uint32_t kAbiRevision`, and `ENGINE_ABI_HASH_TYPE` folds it into the hash
- [x] a kit built against an older revision is refused at load, and the message names the component and its migration guide in `docs/guides/`
- [x] a golden-file test records each kit-ABI component header with its revision: editing such a header fails until the change is recorded as "revision bumped" or "comment only", so a meaning change cannot pass as an ordinary edit
- [x] `CollisionEvents` is bumped for WO-048's change, and `docs/guides/` gains the migration note (empty lists mean "no contact this tick")
- [x] `collision_events.h`'s top comment ("Removed when no events remain") is corrected; it contradicts the rule written below it
- [x] mutation: a header edit with no golden update fails the test; a kit built at the old revision is refused

## Contract
Adds to `kit-abi`'s **Errors**: a kit built against an older MEANING of a shared component is refused by name, as an older layout already is.

Nothing: a component with no `kAbiRevision` is revision 0, so a kit built before this order loads only if no component it uses has been bumped since.

## Not in scope
The `CollisionEvents` redesign itself (WO-052).

## Log
- 2026-10-06:
  - `ENGINE_ABI_COMPONENTS` in `game_module.h` is the one list; the hash and the revision table both expand it. A revision is folded into the hash only when it is not 0, so a kit from before this order keeps its hash unless a component it was built against has been bumped since. `CollisionEvents` has, so every older kit is refused.
  - Naming: modules now export `engineModuleComponentRevisionsV1` (from `ENGINE_GAME_MODULE`), and the loader compares it with its own table when the hash differs. A kit without the export is read as revision 0 everywhere, which is what lets it be refused BY NAME too: "Built against an older meaning of CollisionEvents (kit revision 0, engine revision 1) ... docs/guides/kit-abi-revisions.md". The `EngineGameModuleV1` table is unchanged (its size is checked exactly), so this is a new optional symbol, as `engineModuleContractsV1` was.
  - `scripts/kit_abi_headers.py` (ctest `kit_abi_headers`) records each header's text hash, its code hash with comments stripped, and its revision in `tests/fixtures/kit_abi_headers.txt`. `--record --as comment-only` is refused if the code hash moved; `--as revision-bumped` is refused unless the revision went up. There is no third way to record a code edit.
  - Mutations, each checked: a comment added to `camera.h` fails `--check` and records only as comment-only; renaming `Camera::fov` fails, and neither record is accepted; the same edit with `kAbiRevision = 1` records as revision-bumped. The old-revision kit is the `old_revision` gate fixture (hash without revisions, no export); `module_abi_conformance` asserts it is refused and named.
