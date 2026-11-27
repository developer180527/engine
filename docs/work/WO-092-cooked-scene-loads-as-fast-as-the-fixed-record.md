---
status: plan
id: WO-092
title: The cooked scene loads as fast as the fixed record did — a schema-driven binary payload, read straight into components
program: assets
priority: P1
size: M
state: active
depends: [WO-091]
contracts: [scene-service]
touches:
  - src/assets/cooked_scene.h
  - src/assets/cookers/scene/scene_cooker.cpp
  - src/scene/component_schema.h
  - src/scene/entity_serializer.h
  - src/runtime/services/scene_service.cpp
source: WO-091's measurement, declined by the owner 2026-11-26
---
## Why
WO-091 made the shipped player read every component through the schema registry, at a price: 50 000 entities load in 131 ms, where the fixed record (format 3) took 32 ms with today's mesh cache (`tests/scene_load_bench.cpp`, release, Noop backend). The owner did not accept the difference. Nearly all of it is the JSON tree: 56 ms decoding CBOR into nlohmann's DOM (`std::map` objects, one allocation per node through the engine allocator) and most of the rest building and freeing it while components are read out of it.

The registry already knows every field's type, offset-free accessor and default. So the cook can write each registered component as packed binary by its schema, and the player can read those bytes straight into the component, with no JSON tree in between. Only what the registry cannot describe (unknown components, kits' reflected blocks, data at another revision) needs a self-describing form, and it is rare.

## Done when
*Reworded 2026-11-26, when the work started: the design changed, see the log.*
- [x] the shipped player reads the cooked scene's CBOR in place, with no JSON tree: registered components straight into the component, a JSON value built only for what has no in-place reader (a codec of its own other than the mesh's, an unknown key, a kit's block, a revision marker); bytes that are not what the cook writes are rolled back and read through the tree, which keeps nlohmann's rules (2026-11-26: `src/assets/cbor_reader.h`, `createEntityCbor`, `SceneService::loadScene`)
- [x] the in-place and JSON readers share every field reader, written once over one value interface, so the two cannot disagree; the JSON path stays for `.scene` files and the editor (2026-11-26: `JsonValue` and `CborValue` in `component_schema.h`; the mesh reference's disk reader too)
- [x] `tests/scene_load_bench.cpp` at 50 000 entities within the measurement's noise of format 3's 32 ms on the same machine (format 3's loader rebuilt for the comparison, as WO-091 did), and the numbers recorded here
- [x] `cooked_scene_test` passes unchanged: every component, kits' and unknown ones included, still reaches the game and saves back as authored (2026-11-26)
- [x] the readers are fuzzed against each other: one random entity, through both, saved results identical (2026-11-26: `fuzz_scene_reader_parity_test`, 20 000 cases clean; making the CBOR adapter treat floats as non-numbers fails it); both scene fuzzers pass on the in-place reader

## Contract
Nothing: `.scene` files do not change, and cooked scenes are re-cooked.

## Not in scope
The editor's `.scene` load time (text JSON); entity creation itself, which format 3 paid too.

## Log
- 2026-11-26, the design changed before any code: the order as first written had the cooker pack each component by its schema (a format 5). The cooker is in `src/assets` and the schemas in `src/scene`, which depends on assets, so that is the module cycle WO-091 removed. And the profile said the cost was nlohmann's JSON tree, not the CBOR bytes. So the bytes stay format 4 and the player reads them in place.
- 2026-11-26, four of five done; the load time is 50 ms against 32.

  | 50 000 entities, release, Noop, this Mac | load |
  |---|---|
  | format 3, its loader rebuilt from git against today's AssetService | 32.1–32.6 ms |
  | format 4 through the JSON tree (WO-091 as closed) | 131 ms |
  | + the in-place reader | 92 ms |
  | + float arrays in one pass, faster heads, the mesh marker only when needed | 77 ms |
  | + one pass, no validation walk, rollback to the tree on failure | 57 ms |
  | + no second walk of each value, mesh reference read in place | 52 ms |
  | + a pending skin and a resolved mesh known without component lookups | 49.8–52.3 ms |

  Tried and reverted: the unresolved-mesh marker made lazy at first (it lost a reference that a malformed load threw on; done later with a catch), and creating each entity in its final archetype with `ecs_entity_desc_t.add` (52.5 ms: the extra walk and default construction cost more than the archetype moves saved).

  Where the 18 ms gap is, by sampling both loaders the same way: about a quarter of either load is `safeReparent` (flecs makes an archetype per parent; the same in both). Format 3 read a fixed record with a `memcpy`; format 4 walks 9.4 MB of CBOR, about 10 ms of the gap, and the rest is per-entity work around the flecs calls (the registry walk, std::function per field). Closing it needs the walk itself gone, which is a format that is not self-describing: the packed records this order first described, with the cycle solved by the scene layer registering the packer with the cooker.
- 2026-11-27, the packed payload built (`src/assets/packed_scene.h`, encoding 2): each registered component the cook can pack is written by its schema's fields and copied back at load; the rest rides as CBOR, read in place. The cooker takes the scene layer's packer (`EntitySerde::scenePacker()`), installed by engine_cook (now linking flecs only: no bgfx, GLFW, Lua or Jolt symbols), the editor's CookService and `SceneSerializer::cookScene`; with none it writes CBOR. A file cooked under another revision of a schema is rebuilt as JSON from its own field table and migrated. **50 000 entities: packed 36.5–37.3 ms, CBOR 51.5–52.7 ms, format 3 32 ms**; file 7.5 MB. The parity fuzzer covers all three readers (20 000 cases); the service fuzzer corrupts packed and CBOR payloads and found BUG-0081 (invalid UTF-8 threw out of a load, reachable since WO-091). Still open: the last ~5 ms (the per-entity flecs work and std::function calls), so the item stays unticked.
- 2026-11-27, **the last gap closed: 29.7–31.3 ms, against format 3's 32.1–32.6.** A sample of `scene_load_bench --profile` (35.4 ms on today's tree) put most of the load in flecs' table moves: `ecs_cpp_set` -> `flecs_ensure` -> `flecs_commit`. Each component set moved the entity to a new table, four or five times per entity (EntityId, name, transform, rigid body, light, then the mesh from the CBOR rest). The packed records already say which components an entity has, so `createEntityPacked` now finds the final table through flecs' cached graph (`ecs_table_add_id`, from `EntityId` and each record's component) and creates the entity there (`ecs_new_w_table`). Each set then writes in place. Only the components a record's `unpack` sets are pre-added (`ComponentSchema::packedComponent`): the generic schema and Name, each of which sets exactly its own component on success, and a failure rolls the whole load back, so no default value the scene never had can be left behind. The mesh, read from the CBOR rest, still moves once.

  | 50 000 entities, release, Noop, this Mac | load |
  |---|---|
  | format 3 (2026-11-26, its loader rebuilt from git) | 32.1–32.6 ms |
  | packed, before (today's tree) | 35.0–35.5 ms |
  | **packed, made in the final table** | **29.7–31.3 ms** |
  | CBOR (unchanged: there the component list needs a walk, which cost more, see above) | 52.7 ms |

  Format 3 was not rebuilt again today. Its number is yesterday's on the same machine, and the packed loader before this change measured within 0.5 ms of yesterday's 36.5–37.3, so the machine reads the same. Correctness: the 30 scene, cooked-scene, reader-parity fuzz and determinism tests pass, and so does the whole unit lane (136).
