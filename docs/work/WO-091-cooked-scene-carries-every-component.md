---
status: plan
id: WO-091
title: The cooked scene carries every component — the shipped player loads through the schema registry, not a fixed 256-byte record
program: assets
priority: P1
size: M
state: todo
depends: [WO-071]
contracts: [scene-service]
touches:
  - modules/assetlib/include/assetlib/scene_asset.h
  - src/assets/cookers/scene/scene_cooker.cpp
  - src/runtime/services/scene_service.cpp
source: found while finishing WO-071, 2026-11-26 (read: scene_asset.h:54, scene_cooker.cpp:190-290, scene_service.cpp:119-286; the C API starts the player from `scenes/<name>.cooked`, eng_capi.cpp)
---
## Why
The editor saves and loads scenes through the schema registry (WO-071), which keeps every component, kits' and unknown ones included. The shipped player does not read that format. `engStart` loads `scenes/<name>.cooked`, a binary scene whose only per-entity data is a fixed 256-byte `assetlib::SceneEntity` with slots for nine components: transform, name, mesh, camera, rigid body, script, character controller, light and animator (`scene_asset.h:54`). `cookSceneFile` writes only those, and `SceneService::loadScene` reads only those.

So in a shipped game, today:
- **colour grading is gone.** A camera's exposure, tone mapper and LUT (`colourGrading`) have no slot, so every shipped game renders with the defaults, whatever the editor showed;
- **kits' components never arrive.** `reflected` data (`combat::Health`) is not cooked, so a game whose kits keep state in components starts with none of it;
- **unknown components and revisions are dropped.** What WO-071 keeps (DR-0034) the cook throws away, and a component saved at a later revision is read by the cooker's hand-written fields as if it were revision 0;
- **a second hand-written list.** Adding a component means editing the registry, the cooker and the loader, and forgetting the last two passes every test the editor's path has.

The fixed record is fast to read, and the scene's load time has been measured before (`scene_service.cpp`: the id index). The fix keeps that: the component data goes through the registry, and what the fast path needs (mesh paths to preload) stays indexed.

## Done when
- [ ] the cooked scene carries each entity's components in the registry's saved form (the same JSON `saveEntity` writes, or a binary encoding of it with the same content), next to what the loader needs indexed (cooked mesh paths, for preload)
- [ ] `SceneService::loadScene` creates entities through `EntitySerde::createEntity` with the registry, resolving cooked meshes through `SceneAssets`, so the editor and the player read one format; the fixed per-component branches are gone
- [ ] a test cooks a scene with every component, colour grading, a kit's reflected component and an unknown one, loads the cooked file through `SceneService`, and finds each on its entity, then saves the world and compares with the source scene
- [ ] load time measured on a 50 000-entity scene before and after (`scene_service.cpp`'s case), and no slower than the fixed record by more than the measurement's noise, or the difference stated and accepted
- [ ] the cooked format's version bumped; an older `.cooked` is refused at load by name and re-cooked (it is derived data: the source scene is the truth)

## Contract
Nothing: a project's `.scene` files do not change. Cooked scenes are build output and are re-cooked.

## Not in scope
The variable-length blob storage (WO-073); prefabs.
