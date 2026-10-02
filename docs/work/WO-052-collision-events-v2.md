---
status: plan
id: WO-052
title: CollisionEvents v2 — a plain-data view into an engine-owned contact stream
program: providers
priority: P1
size: M
state: todo
depends: [WO-051]
contracts: [kit-abi, engine-plugin, script-services]
touches:
  - src/components/collision_events.h
  - src/plugins/jolt_plugin.h
  - src/plugins/lua_script_plugin.h
  - include/engine/game_module.h
source: review 2026-10-03 of WO-048 (b39bbed)
---
## Why
`CollisionEvents` carries two `std::vector`s across the kit boundary, and "no contact this tick" is only an empty list, which kits must know to check.

A kit compiled against a different standard library, or a different debug setting, reads those vectors wrong. On Windows a kit that grows one allocates from its own runtime's heap and the engine frees it from another: heap corruption. That hazard predates WO-048; the ambiguity is WO-048's.

## Done when
- [ ] the engine keeps one contact stream per world: plain records (self, other, begin/end, physics step, point, normal, impulse), each pair recorded in both directions, sorted so an entity's events are contiguous and the order is deterministic
- [ ] `CollisionEvents` becomes plain data, a view into that stream: `{ uint64_t tick; uint32_t first; uint16_t entered, exited; }`. No `std::vector` crosses the kit boundary
- [ ] the component is still added once and never removed, so a tick makes no structural change (WO-048's gain kept)
- [ ] kits read it with an inline helper (`forEachContact(world, e, fn)`) and a C ABI call (`engineContacts(world, &events, &count)`). The stream is valid until the next physics step, and the contract says so
- [ ] within one frame of several physics steps, a contact that begins and ends keeps its order (the step index)
- [ ] Lua's `onCollisionEnter` / `onCollisionExit` are unchanged and read the stream; the collision-flush query is built once, not every tick
- [ ] the determinism hash covers the stream
- [ ] `kAbiRevision` bumped (WO-051) and the migration guide updated
- [ ] tests: a quiet tick reports nothing; structural changes per tick stay 0; multi-step order; a v1 kit is refused; the stream view is fuzzed. Each red with its fix removed

## Contract
Changes `kit-abi`: `CollisionEvents` becomes plain data with a stated lifetime for what it points at.

Nothing: before the first contact an entity has no `CollisionEvents`, and `engineContacts` returns zero records. A world with no physics provider has an empty stream.

## Not in scope
Contact "stay" (persisting) events; add them when a game asks.
