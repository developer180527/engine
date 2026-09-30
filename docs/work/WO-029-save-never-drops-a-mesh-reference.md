---
status: plan
id: WO-029
title: Saving a scene never drops a mesh reference it failed to load
program: assets
priority: P0
size: S
state: todo
contracts: [scene-service]
touches:
  - src/scene/entity_serializer.h
  - src/scene/scene_serializer.h
  - tests/fuzz_entity_serde_test.cpp
source: conversation 2026-09-30 — fps_shooter's house, car and pistol lost their meshes
---
## Why
A model whose mesh fails to load (or is still loading) gets no `MeshRenderer`, so the next save writes that entity without its mesh reference, and the model is gone for good.

This happened in `fps_shooter`: `house`, `covered_car_2k` and `pistol_without_mag (2)` are saved with only a name and a transform. They still show in the hierarchy and the gizmo moves them, but nothing renders, and no log says why. The project is untracked, so there was nothing to restore from.

`loadMesh` returns without a component when a reference is unresolved, a cooked load fails, or an Assimp load is still pending. `saveMesh` only writes what the component holds. So a failed load erases the reference on the next save.

## Done when
- [ ] a mesh reference that could not be loaded is kept on the entity, as data the loader did not resolve, and `saveMesh` writes it back unchanged
- [ ] the same holds for an async (Assimp) load that has not finished when the scene is saved
- [ ] the editor shows such an entity as "mesh missing: <path>" in the Inspector, instead of looking like an empty entity
- [ ] a round-trip test: load a scene whose mesh reference points at a missing file, save it, and the saved JSON still has the original reference
- [ ] mutation: dropping the kept reference in `saveMesh` turns the test red

## Contract
Adds to `scene-service`'s **Errors**: loading never deletes authored data. A reference that cannot be resolved stays in the document and is reported.

Nothing: before this lands, the loader logs the failure (it already warns for unresolved references) and the entity renders nothing, as today. The fix only changes what a save writes.

## Not in scope
- Why these three models failed to load. That is WO-002/WO-014 for the skinned pistol; the FBX files need their own check once this order makes the failure visible.
- Recovering fps_shooter's lost references. They must be re-placed by hand.
