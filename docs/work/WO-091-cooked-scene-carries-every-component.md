---
status: plan
id: WO-091
title: The cooked scene carries every component — the shipped player loads through the schema registry, not a fixed 256-byte record
program: assets
priority: P1
size: M
state: active
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
- [x] the cooked scene carries each entity's components in the registry's saved form (the same JSON `saveEntity` writes, or a binary encoding of it with the same content), next to what the loader needs indexed (cooked mesh paths, for preload) (2026-11-26: format 4, assetlib's header and the scene as CBOR, `src/assets/cooked_scene.h`; mesh paths are derived from the payload by `meshPaths()` rather than stored apart, for preload and the packager, LOD levels included)
- [x] `SceneService::loadScene` creates entities through `EntitySerde::createEntity` with the registry, resolving cooked meshes through `SceneAssets`, so the editor and the player read one format; the fixed per-component branches are gone (2026-11-26; the skin is wired by one function, `wireSkin`, for the editor's import callback, a cooked mesh in the editor and the player)
- [x] a test cooks a scene with every component, colour grading, a kit's reflected component and an unknown one, loads the cooked file through `SceneService`, and finds each on its entity, then saves the world and compares with the source scene (2026-11-26: `tests/cooked_scene_test.cpp`, 10 checks, a newer-revision camera included)
- [ ] load time measured on a 50 000-entity scene before and after (`scene_service.cpp`'s case), and no slower than the fixed record by more than the measurement's noise, or the difference stated and accepted
- [x] the cooked format's version bumped; an older `.cooked` is refused at load by name and re-cooked (it is derived data: the source scene is the truth) (2026-11-26: version 4; a version-3 file is refused with "re-cook the scene", cooker_test)

## Contract
Nothing: a project's `.scene` files do not change. Cooked scenes are build output and are re-cooked.

## Not in scope
The variable-length blob storage (WO-073); prefabs.

## Log
- 2026-11-26, four of five done; the load time is the owner's call.
  - **Load time, measured** (`tests/scene_load_bench.cpp`, release, Noop backend, M-series Mac; 50 000 entities, half with one shared mesh, one in ten a light, one in five a rigid body; median of 5):

    | | format 3 (fixed record) | format 4 (registry) |
    |---|---|---|
    | before the mesh-cache fix below | 438 ms | 541 ms |
    | after it | 32 ms (throwaway build of the format-3 loader against today's AssetService) | 131 ms, 56 ms of it decoding CBOR into JSON |
    | file | 13 124 KB | 9 388 KB |
    | cook | 190 ms | 157 ms |

    So like for like, format 4 is about 100 ms slower at 50 000 entities (2 µs an entity), nearly all of it allocation building and freeing nlohmann's JSON tree. Against what shipped before today it is 3.3 times faster, because of the cache fix. The way back to format 3's speed, if a level's load needs it: a schema-driven binary encoding the registry reads straight into components, no JSON tree. Not built.
  - Found and fixed on the way: BUG-0077 (every instance after the first of a shared skinned mesh had no skeleton, in the player and the editor's cooked path; the same cache fix), BUG-0078 (WO-071's registry read a rigid body without bodyType as dynamic), BUG-0079 (the editor's cooked path never wired a skin), BUG-0080 (an unresolved animator clip was erased on save).
  - The editor and the player used to disagree on two hand-written cases, now one rule each (`docs/reference/scene-format.md`): a component whose value is not an object is skipped (the player attached a default one), and an entity without an id gets a fresh one (the player gave it 0).
  - A skinned mesh with no authored Animator now plays its first clip in the player, as the editor always showed; the player used to leave it stopped.
  - A mesh that fails to load in the player now shows the placeholder cube where a primitive library is present, as in the editor (WO-018); SceneService is given none today, so nothing changes there yet.
  - Fuzzers moved to generator 2 (format 4); their seeds, found under 1, are kept and replayed. New: CBOR nested past 64 levels is refused, not recursed into.
