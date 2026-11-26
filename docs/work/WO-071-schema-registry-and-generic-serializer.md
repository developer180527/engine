---
status: plan
id: WO-071
title: A component schema registry and one generic scene serializer, with unknown components kept
program: assets
priority: P1
rank: 3
size: L
state: active
depends: []
contracts: [scene-service]
touches:
  - src/scene/entity_serializer.h
  - src/scene/scene_serializer.h
  - src/scene/reflected_serde.h
  - src/components/meta_registry.h
source: docs/plans/engine-architecture.md §4 and §8 step 2; roadmap §2.8; DR-0034
---
## Why
Every later step names components by registered schema: the change stream, the C world API, provider-namespaced components, migrations. Today the scene serializer is a hand-written list (`entity_serializer.h`, 11 schemas hard-coded in `meta_registry.h`) that drops any component it does not know. (`MeshRenderer` saving registry ids was true only of the in-memory snapshot; on disk it already wrote asset references, checked 2026-11-26.)

## Done when
- [ ] a schema registry: name (namespaced, `core.Transform`), revision (`kAbiRevision`), fields with the draft's field types (`eng_world.h`, including the proposed ones), defaults, and an optional migration per revision
- [x] every existing component registered through it; one generic serializer replaces the hand-written list (2026-11-26: 12 schemas in `registerBuiltinSchemas`; 8 field-by-field, 4 with codecs of their own: mesh, LOD chain, skinned mesh, animator)
- [x] **every existing scene and project round-trips byte-identically** through the new serializer (2026-11-26: `tests/fixtures/scenes/every_component.{disk,memory}.json`, written by the hand-written table at 7807d9d, byte-identical from the registry and through load then save; the two scenes on disk, `fps_shooter`'s and `experiments/libgui_sandbox`'s, produce the same bytes from both serializers and equal their files. No sample or tracked project has a scene; the WO-033 check project is not on this machine)
- [x] unknown components load as opaque data and save back unchanged; a test loads a scene with a component no one registered, saves it, and compares (2026-11-26: `scene_schema_test`, disk and memory, plus data from a newer revision; mutation-checked: dropping the re-emit fails 3 checks)
- [x] a migration: one component's revision bumped in a test, an old scene loads through its migration, and a scene saved before the bump is kept as a fixture (DR-0034) (2026-11-26: `test.Thruster` 0 to 1, `tests/fixtures/scenes/thruster_rev0.json` written by the revision-0 schema; a missing migration keeps the data unchanged)
- [x] no GPU handle in any saved component: `MeshRenderer`'s registry ids are replaced by asset UUIDs on disk (2026-11-26: already so on disk, `saveMesh` writes an AssetRef and a material name; the ids are only in the in-memory snapshot, which never leaves the process. Now asserted: the disk fixture has none of the five id keys, and the memory one has them, so the check can see them)
- [ ] the project file declares extensions as required or used; a missing required one fails at load by name (DR-0035)

## Contract
Nothing: scenes on disk keep their current shape (byte-identical round-trip); the in-memory components do not change.

## Not in scope
The variable-length `EngBlob` storage (WO-073 builds it with the world API); renaming components to the core vocabulary (a migration of its own, later).

## Log
- 2026-11-26, started; four of seven done.
  - `scene/component_schema.h`: `ComponentSchema`, `SchemaRegistry` (refuses a taken key or name, or the same component at another revision, naming both), `SchemaBuilder<T>` (f32, float arrays, bool, enum, u8, string), `migrateToCurrent`. `scene/unknown_components.h` keeps what cannot be read.
  - `entity_serializer.h`: the table is `registerBuiltinSchemas`; `saveEntity`/`createEntity` walk the registry; `SerdeContext::schemas` lets a test use its own. The undo stack's snapshot, restore and remove go through it too.
  - Fixtures first, from the old code, then the swap; `scene_schema_test` (26 checks). Unit lane 128/128.
  - Behaviour change, for malformed input only: a wrong-typed bool or enum keeps its default and the rest of the component loads, where before the whole component was skipped.
  - Left: the field types beyond those six (the draft's integers, f64, entity, asset, mat4) and per-field defaults stated in the schema rather than taken from the struct; the project's required and used extensions (DR-0035).
