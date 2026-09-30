---
status: plan
id: WO-048
title: Collision events without archetype churn; Sim.post stops growing faster than the world
program: providers
priority: P1
size: S
state: todo
touches:
  - src/plugins/jolt_plugin.h
  - src/plugins/lua_script_plugin.h
source: simulation profile 2026-10-01 (tests/perf/sim_profile, build-prof RelWithDebInfo)
---
## Why
`sim_profile` measured `Sim.post` (the post-physics broadcast) at **9.6 us per tick at scale 1 and 450 us at scale 2**: 47x for twice the world. It is the only phase that grows super-linearly. The `Core` heap's allocations grow the same way, from 329 000 to 2.5 million over the run (7.7x).

The sampling profile shows why. `JoltPlugin::flushCollisionEvents` ADDS a `CollisionEvents` component to every body that has contacts this tick and REMOVES it from bodies that stopped. In flecs a structural add or remove moves the entity to another archetype table: a copy of all its components, plus a heap allocation for the event vector's constructor. So every body in contact moves tables twice per tick, and the cost tracks the number of contacts, which in piles grows faster than the number of bodies. `LuaScriptPlugin::onPostPhysics` then iterates what that churn produced.

## Done when
- [ ] a body's collision events reach scripts with no structural change per tick: the component is kept and cleared in place (or events go to one per-world buffer indexed by entity), so no table moves and no per-tick allocation for the vector
- [ ] `sim_profile 2` shows `Sim.post` scaling linearly with the world (target: under 50 us at scale 2 on the reference machine), and the `Core` allocation count grows linearly between scale 1 and 2
- [ ] scripts see exactly the events they saw before, in the same order: the determinism gate stays at 0 divergences (CollisionEvents is hashed), and a test pins event delivery across a tick with and without contacts

## Contract
Whatever reads `CollisionEvents` (scripts, kits through the ABI) sees the same events in the same order.

Nothing: a body with no contacts has an empty event list, instead of no component.
