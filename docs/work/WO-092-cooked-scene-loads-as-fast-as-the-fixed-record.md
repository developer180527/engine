---
status: plan
id: WO-092
title: The cooked scene loads as fast as the fixed record did — a schema-driven binary payload, read straight into components
program: assets
priority: P1
size: M
state: todo
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
- [ ] format 5: per entity, its id, parent and each registered component as the schema's fields in binary (by schema name and revision, so a schema change is detected, not misread); anything else as a CBOR blob carried through unchanged
- [ ] the loader reads registered components into the struct directly, through the same field definitions the JSON path uses, so the two cannot disagree; the JSON path stays for `.scene` files and the editor
- [ ] `tests/scene_load_bench.cpp` at 50 000 entities within the measurement's noise of format 3's 32 ms on the same machine (format 3's loader rebuilt for the comparison, as WO-091 did), and the numbers recorded here
- [ ] `cooked_scene_test` passes unchanged: every component, kits' and unknown ones included, still reaches the game and saves back as authored
- [ ] both scene fuzzers cover format 5: field data that is short, long or of the wrong schema revision is refused or kept, never misread; format 4 is refused by version and re-cooked

## Contract
Nothing: `.scene` files do not change, and cooked scenes are re-cooked.

## Not in scope
The editor's `.scene` load time (text JSON); entity creation itself, which format 3 paid too.
