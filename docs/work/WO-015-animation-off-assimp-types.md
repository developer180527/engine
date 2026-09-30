---
status: plan
id: WO-015
title: Animation takes engine types, not Assimp types
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: audit IMP-01 (no src/animation entries left in the baseline); the 12 Mixamo zombie clips bit-identical through the new path (350,400 joint-matrix floats); clip_binding_test, anim_pose_test, mesh_backend_test, frontend_assimp_test green
depends: [WO-013]
touches:
  - src/animation/ozz_bridge.h
  - src/animation/assimp_skeleton_loader.h
  - src/animation/clip_library.h
  - src/assets/anim_from_scene.cpp
  - src/assets/clip_source.cpp
  - src/assets/import/frontend_assimp_skeleton.h
source: review 2026-09-29 C3
---
## Why
`buildOzzClip(aiAnimation*)` and `extractSkeleton(aiScene*)` tie the animation module to one parser.

## Done when
- [x] removes `src/animation/assimp_skeleton_loader.h` and `src/animation/ozz_bridge.h` (and `clip_library.h`) from audit IMP-01's baseline (`scripts/audit_baseline.json`); the rule then holds for it without debt
- [x] `buildOzzClip` and `extractSkeleton` take `ImportedScene` clip and skeleton types: `imp::buildOzzClip(const imp::Clip&, const Skeleton&)` and `imp::toAnimSkeleton(const imp::Skeleton&)`, in `assets/anim_from_scene.h`
- [x] `assimp_skeleton_loader.h` is gone, or it lives inside the Assimp front end: it is `assets/import/frontend_assimp_skeleton.h`, namespace `imp::assimp`
- [x] no Assimp include in `src/animation/` (audit rule)
- [x] existing animation tests are unchanged and green (`clip_binding_test` and `anim_pose_test` gain one line each: they install the source reader, as the runtime does)

## Log
- 2026-09-30:
  - **The conversion lives in `src/assets/anim_from_scene.{h,cpp}`.** It is
    the back end's own code (`toAnimSkeleton`, the clip loop), moved and
    shared. Not in `src/animation/`, which must not depend on the import
    stack (assets already depends on animation). Not in
    `src/assets/import/` either, whose non-front-end files may include only
    the standard library (LAYER-05).
  - **`ClipLibrary` no longer parses anything.** An uncooked clip comes from
    a source reader the host installs. The runtime installs
    `imp::readSourceClip` when it has source importers; with none, as in
    shipping and server builds, an uncooked clip is an error, as it was in
    shipping. A clip is now read by the same front end as the character it
    animates, and glTF clips work too.
  - **Behaviour change:** a server build compiled WITH source importers used
    to parse FBX clips through `ClipLibrary`. It now reads cooked clips only,
    matching `runtime_boot.cpp`'s stated policy for servers.
  - **Checked against the old path on real data:** the 12 Mixamo zombie
    clips bound to the Warzombie skeleton, every joint's model matrix at 25
    times per clip (350,400 floats), were bit-identical before and after,
    with 52 of 52 tracks each.
