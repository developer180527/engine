---
status: plan
id: WO-071
title: A component schema registry and one generic scene serializer, with unknown components kept
program: assets
priority: P1
rank: 2
size: L
state: todo
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
Every later step names components by registered schema: the change stream, the C world API, provider-namespaced components, migrations. Today the scene serializer is a hand-written list (`entity_serializer.h`, 11 schemas hard-coded in `meta_registry.h`) that drops any component it does not know, and `MeshRenderer` saves the bgfx renderer's registry ids.

## Done when
- [ ] a schema registry: name (namespaced, `core.Transform`), revision (`kAbiRevision`), fields with the draft's field types (`eng_world.h`, including the proposed ones), defaults, and an optional migration per revision
- [ ] every existing component registered through it; one generic serializer replaces the hand-written list
- [ ] **every existing scene and project round-trips byte-identically** through the new serializer (the samples, the test fixtures, the WO-033 check project)
- [ ] unknown components load as opaque data and save back unchanged; a test loads a scene with a component no one registered, saves it, and compares
- [ ] a migration: one component's revision bumped in a test, an old scene loads through its migration, and a scene saved before the bump is kept as a fixture (DR-0034)
- [ ] no GPU handle in any saved component: `MeshRenderer`'s registry ids are replaced by asset UUIDs on disk
- [ ] the project file declares extensions as required or used; a missing required one fails at load by name (DR-0035)

## Contract
Nothing: scenes on disk keep their current shape (byte-identical round-trip); the in-memory components do not change.

## Not in scope
The variable-length `EngBlob` storage (WO-073 builds it with the world API); renaming components to the core vocabulary (a migration of its own, later).
