---
status: plan
id: WO-016
title: Offline clip cooker — no clip is cooked at runtime
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: clip_cook_test (a clip nothing bound cooks, binds in the editor with no source reader, ships and binds with no registry, poses bit-identically to the source; damage refused); cooker_test animation-only row; fuzz_cooked_clip_test (regress + explore, 20,000 clean)
depends: [WO-015]
contracts: [cooker]
touches:
  - src/animation/clip_library.h
  - src/animation/cooked_clip.h
  - src/assets/cookers/clip/clip_cook.cpp
  - src/assets/cookers/mesh/mesh_cooker.cpp
  - src/tools/packaging/package_closure.cpp
  - src/tools/engine_build.cpp
  - src/runtime/runtime.cpp
source: review 2026-09-29 C4, R2
---
## Why
A standalone clip, such as a Mixamo FBX, is cooked only the first time the editor binds it.

A shipped build that uses a clip nobody played in the editor fails with "clip not cooked". Whether it works depends on what someone clicked.

## Done when
- [x] removes `src/animation/clip_library.h` from audit IMP-01's baseline (`scripts/audit_baseline.json`); the rule then holds for it without debt (done with WO-015, which took its Assimp parse out)
- [x] a clip cooker in the normal cook pipeline, fingerprinted like the others: the mesh cooker's output for an animation-only source (same DDC key, registry record and cooker version, now 21), an `anim::CookedClip`
- [x] the cook-on-first-bind path in `clip_library.h` is removed: `ClipLibrary` reads cooked clips and writes nothing; the per-skeleton `.ozzclip` format is gone
- [x] the "animation-only, skipping cook" branch in the mesh cooker routes to the clip cooker instead (`MeshCooker::cook`; the mesh back end on its own still skips such a scene, which `mesh_backend_test` pins)
- [x] a test: a project whose clip was never bound in the editor still plays it in a cooked-only runtime (`clip_cook_test` §3: packaged, no registry, no source reader)

## Contract
Nothing: in a runtime, a missing cooked clip is a WO-018 pending job, not an inline cook.

## Log
- 2026-09-30:
  - **The cooked clip is skeleton-independent: keys by bone name.** An ozz
    Animation is compiled against one skeleton's joint order, and a cooker
    cannot know which character a clip will play on; that is why the old
    cache was per TARGET skeleton. So the cook stores an
    `offline::RawAnimation` plus its track bone names (`animation/cooked_clip.h`),
    and `ClipLibrary` binds by name at load through `anim::bindRawClip`, the
    same code `imp::buildOzzClip` now uses. A clip bound from its cooked file
    poses bit-identically to one built straight from the source.
  - **Finding a cooked clip.** A cooked scene names a clip by its SOURCE
    path, and a dist has no registry. So `engine_build` copies each cooked
    clip (every one: a script may play any) to
    `.cache/anim/<assetlib::packagedClipFileName(source)>`, and `ClipLibrary` looks
    there by default; in a project, the runtime installs a locator that asks
    the registry. Changing the scene format to carry a cooked clip path, as
    it does for meshes, was the alternative; `SceneEntity` has no spare
    bytes, and this needed no format change.
  - **The format is guarded like the mesh's ozz blobs:** a digest checked
    before ozz reads a byte, a GuardedStream, and checks after decoding.
    `fuzz_cooked_clip_test` damages the whole file (the digest must refuse
    it) and, with the digest recomputed, the engine's own name table (its
    bounds checks must refuse it): 20,000 iterations clean.
  - **The WO-015 source reader stays** as the dev fallback for a clip
    dropped into a project before its cook finishes. WO-018 turns that into
    a pending job.
  - **Two test headers pinned names.** `tests/import_contract.h` and
    `gltf_writer.h` now say `using imp::Skeleton; using imp::Bone;`:
    `clip_cook_test` is the first file to include both them and the engine's
    animation types, which share those names.
