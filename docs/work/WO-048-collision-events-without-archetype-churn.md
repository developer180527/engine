---
status: plan
id: WO-048
title: Collision events without archetype churn; Sim.post stops growing faster than the world
program: providers
priority: P1
size: S
state: done
done: 2026-10-01
evidence: collision_events_test (delivery to scripts; 0 archetype moves after the first contact, 219 with the old removal); sim_profile Sim.post 443 -> 11 us at scale 2; determinism gate 0 divergences; BUG-0071
touches:
  - src/plugins/jolt_plugin.h
  - src/plugins/lua_script_plugin.h
source: simulation profile 2026-10-01 (tests/perf/sim_profile, build-prof RelWithDebInfo)
---
## Why
`sim_profile` measured `Sim.post` (the post-physics broadcast) at **9.6 us per tick at scale 1 and 450 us at scale 2**: 47x for twice the world. It is the only phase that grows super-linearly. The `Core` heap's allocations grow the same way, from 329 000 to 2.5 million over the run (7.7x).

The sampling profile shows why. `JoltPlugin::flushCollisionEvents` ADDS a `CollisionEvents` component to every body that has contacts this tick and REMOVES it from bodies that stopped. In flecs a structural add or remove moves the entity to another archetype table: a copy of all its components, plus a heap allocation for the event vector's constructor. So every body in contact moves tables twice per tick, and the cost tracks the number of contacts, which in piles grows faster than the number of bodies. `LuaScriptPlugin::onPostPhysics` then iterates what that churn produced.

## Done when
- [x] a body's collision events reach scripts with no structural change per tick: the component is kept and cleared in place (or events go to one per-world buffer indexed by entity), so no table moves and no per-tick allocation for the vector
- [x] `sim_profile 2` shows `Sim.post` scaling linearly with the world (target: under 50 us at scale 2 on the reference machine), and the `Core` allocation count grows linearly between scale 1 and 2
- [x] scripts see exactly the events they saw before, in the same order: the determinism gate stays at 0 divergences (CollisionEvents is hashed), and a test pins event delivery across a tick with and without contacts

## Contract
Whatever reads `CollisionEvents` (scripts, kits through the ABI) sees the same events in the same order.

Nothing: a body with no contacts has an empty event list, instead of no component.

## Log
- 2026-10-01:
  - **The churn fix.** A body keeps `CollisionEvents` once it has had a
    contact, and each tick the lists are cleared in place and refilled in
    the same sorted order, so the determinism gate shows 0 divergences.
    The per-tick `std::map` of fresh components, and its allocations, are
    gone. The Lua plugin skips bodies with empty lists before its instance
    lookup.
  - **It was half the story.** `Sim.post` fell from 443 to 186 us at scale
    2, still 40x scale 1. Measuring the WORKLOAD, not just the time, showed
    why: 3 111 contact starts and ends per tick at scale 2 against 21 at
    scale 1. Jolt was initialised for 4 096 contact constraints and body
    pairs, 2 000 boxes in piles need about 8 000, and Jolt drops what does
    not fit and says so only in `Update`'s return value, which was
    discarded. So the piles never settled. That is BUG-0071: capacity
    raised (65 536 bodies and pairs, 20 480 constraints, a 64 MB temp
    allocator, since a full one aborts), and overflow now logged, naming
    the constant.
  - **Result (`build-prof`, RelWithDebInfo):**

    | scale | contact starts/ends per tick | `Sim.post` | `Core` allocs |
    |---|---|---|---|
    | 1 | 21 | 5.2 us | 143 000 |
    | 2 | 45 | 11.2 us (was 443) | 163 000 (was 2.6 M) |
    | 4 | 90 | 29.7 us | 187 000 |

    Physics got slightly slower at scale 2 (3.8 ms, was 3.5), because it now
    solves the contacts it used to drop.
  - **The temp allocator's cost, measured.** Resident memory for
    `collision_events_test` is 27.1 MB with 64 MB against 24.0 MB with 16 MB:
    the reservation is committed only as a step touches it.
  - **Nothing tested collision delivery before.** Only the ABI test and the
    determinism gate touched `CollisionEvents`; `collision_events_test` is
    new. Put back the old per-tick removal and it reports 219 archetype
    moves in 240 ticks.
