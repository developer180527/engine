---
status: as-built
tier: hardened
verified: 2026-10-01
parses-external-input: true
covers:
  - src/scene/
tests:
  - tests/kit_lifecycle_test.cpp
  - tests/fuzz_entity_serde_test.cpp  # the JSON deserializer, hostile input
  - tests/scene_parents_test.cpp      # the hierarchy post-pass, hostile input
  - tests/scene_mesh_reference_test.cpp  # a mesh that fails to load is never saved away
  - tests/scene_material_roundtrip_test.cpp  # a material override survives save + load (BUG-0072)
---
# Scene

## Hostile input
`.scene` JSON is the most-edited untrusted input in the tree: people hand-edit
it, merge it, and resolve conflicts in it, so malformed is the NORMAL case, not
the adversarial one. `tests/fuzz_entity_serde_test.cpp` drives `createEntity`
with wrong-typed, short, non-finite and deeply nested values. It found three
bugs, and the order matters — each was hidden behind the one before it:

1. **Any wrong-typed field threw.** nlohmann's `value()` does not fall back to
   the default when a key exists with the wrong type; it throws `type_error`.
   `scene_serializer.h` wraps only `json::parse` in try/catch, so one
   `"fov": "60"` propagated out of scene load. Now every component load is
   individually try/caught: a malformed COMPONENT is skipped and logged, and
   the rest of the entity still loads. The granularity is deliberate — wrapping
   the whole function turns a bad field into a missing entity, and wrapping the
   whole load turns it into a missing scene.
2. **A short float array was undefined behaviour.** `j["position"][0..2]` on a
   two-element array: nlohmann's CONST `operator[](size_type)` is not `at()`,
   has no bounds check, and returns a reference to nothing. Five sites had it.
   It was only reachable once (1) stopped throwing first.
3. **Non-finite values were accepted.** JSON has no NaN literal, but `1e999`
   parses to +inf, and an infinite scale or fov propagates into every world
   matrix downstream — corrupting a frame nowhere near the load that caused it.

`readFloats`/`readFloat` are the result: every float read keeps its default for
a missing, wrong-typed, short, or non-finite value. They now live in
`core/json_read.h`, shared — the identical defect turned up in `editor_prefs.h`
(a live segfault on the project-open path) and `undo_stack.h`, so the helper is
shared rather than copied a fourth time.

Entity creation is only HALF of a scene load. Parent links cannot be restored
during the entity pass — a child may appear before its parent — so
`SceneSerializer::restoreParents` runs afterwards over the SAME untrusted JSON,
and in `loadAsync` it sits OUTSIDE the try/catch that covers only
`json::parse`. It read `id`/`parentId` with `value()`, so `"id": "abc"` in a
hand-edited scene threw straight out of scene load: the editor died on
File→Open, `engine_host` died on boot. Eight distinct shapes reproduced it.
`readEntityId` now accepts any non-negative integer and refuses everything
else, and one malformed record costs ONE link rather than the whole hierarchy —
a scene that opens with everything silently unparented is worse than one that
reports a problem.

One subtlety worth keeping: the check is NOT `is_number_unsigned()` alone.
nlohmann stores a positive literal built in-process from an `int` as SIGNED, so
that spelling rejected valid ids depending only on how the JSON was
constructed — parsed-from-disk and built-in-memory disagreed.

## ColourGrading (2026-09-17)
The colour stage B camera component serializes through the hand-written table as
`"colourGrading"`, after `camera`, and is listed in `reflected_serde.h`'s
hand-written set so the generic path does not save it a second time. Tolerant:
wrong-typed fields keep defaults; an unknown-but-in-range tone mapper id is kept
as read, so a scene from a newer engine round-trips. `fuzz_entity_serde_test`
generates it.

## Purpose
World (de)serialization — the single component serde path that scene save/
load, the play-mode snapshot, and the undo stack all share. Future home of
prefabs.

## Contents
- **`entity_serializer.h`** — the one component serde table. COOKED PATHS ARE
  PASSED RELATIVE: `cookedPath` comes out of the registry as `meshs/<uuid>.cooked`,
  relative to the project's `.cache`, and `AssetService::loadMesh` resolves relative
  paths against exactly that. Until 2026-08-05 this pre-joined the project ROOT,
  producing `<project>/meshs/<uuid>.cooked` — a path that has never existed — so
  every cooked mesh load failed and fell through to the Assimp source importer. It
  looked fine because scenes still rendered, just via the slow path; a shipped dist
  with no source assets would have failed outright. Do not "helpfully" absolutise it
  again. Two modes:
  `Disk` (cross-session: AssetRef uuid+relative refs, semantic clipIndex —
  session handles NEVER hit disk) and `Memory` (same-session snapshot: live
  handle reuse). Add a component once here and every path picks it up.
  `lodMesh` is hand-written for the same reason `meshRenderer` is — its levels are
  asset references — with one limitation stated in place: **levels resolve
  SYNCHRONOUSLY from cooked meshes only.** `loadMesh`'s last resort is an Assimp
  import on a worker that completes by setting a `MeshRenderer`, and no shape of that
  can fill slot 2 of an `LodMesh`. An unresolved level therefore SHORTENS the chain
  (`break`, not `continue` — a gap would shift every coarser level one threshold
  finer), which costs triangles and never correctness.
  Authored `lodMesh` is the manual override. The ordinary case is now automatic:
  `MeshCooker` emits a chain, `AssetService::loadMesh` returns it, and BOTH load
  paths — `entity_serializer` (JSON) and `SceneService` (cooked binary) — set
  `LodMesh` from it. Both, deliberately: wiring only one produced the same asset
  rendering with LOD in the player and without it in the editor.
- **`reflected_serde.h`** — the GENERIC half of serde, driven by flecs meta:
  any component registered with `.member<>()` that isn't in the hand-written
  table saves/loads automatically under the entity's `"reflected"` sub-object,
  keyed by component path (`"combat::Health"`). Types not registered at load
  time (kit components before their kit loads) stash in `ReflectedPending` and
  `applyPending()` applies them once the type appears (called after sim-start
  broadcasts and mid-play kit loads); pending blobs re-emit on save, so data
  round-trips losslessly even with the kit disabled. One meta registration
  drives serde + the generic Inspector section + the + Add Component menu
  (`EditorAddable` tag) + Lua FFI schemas.
- **`scene_assets.h`** — `SceneAssets`, the hooks through which loading and
  saving reach assets: load a cooked mesh (with its LOD levels), a material by
  name, a material's name back from its handle, and stream a source mesh on a
  worker. See "How a scene reaches assets" below.
- **`scene_serializer.h`** — `.scene` JSON save/load on top of the table:
  - `loadAsync` — names/transforms synchronous, meshes stream via
    `SceneAssets::streamMesh`; the completion resolves skeleton/clip handles
    fresh (Animator::clipIndex selects the clip) and applies the authored
    material.
  - `save` — takes the host's `SceneAssets` for material names; without one,
    material overrides are not written.
  - `saveToString`/`loadIntoWorld` — instant play-mode snapshot.
  - `cookScene` — thin wrapper over `assets/cookers/scene_cooker`.

## How a scene reaches assets (WO-047)

`src/scene` includes nothing from `src/runtime`. It used to: the serializers
called `AssetService` and `AsyncLoader` directly, and that was the last edge of
the module cycle (the runtime also includes scene, to load and snapshot
worlds). Now the serializer DESCRIBES what it needs through `SceneAssets`
(`scene_assets.h`, which includes only `core/handle.h`), and the host answers:
`sceneAssetsFor(AssetService*, AsyncLoader*)` in
`runtime/services/scene_assets_host.h` builds it from the runtime's services.
The editor, `engine_host` and `scene_resave` call that. A test passes fakes.

Every hook is optional, and an empty one means "this host cannot": no cooked
loader falls through to the source path, no material hook leaves overrides
unapplied (load) or unwritten (save), and no streamer keeps the reference in
`UnresolvedMesh` with the reason "no loader".

Cost: `std::function` calls, once per asset reference, while a scene loads or
saves, each wrapping work measured in milliseconds. No tick calls them.

Removing the field is what exposed BUG-0072: `save` tested `ctx.assetService`
on a context where nothing set it, so material overrides were never saved.

## Invariants
- Caller runs `assignMissingIds()` before save (set<> is illegal mid-query).
- Parent links restore in a post-pass (all entities must exist first).
- Scene saves auto-cook the binary twin (editor keeps both in sync).

## Loading never deletes an authored mesh (WO-029)

A mesh reference the loader cannot resolve — the file is missing, the cooked
load or glTF import failed, the build has no importer, or an async import has
not finished — used to leave the entity with no `MeshRenderer`. The save writes
only what a `MeshRenderer` holds, so the next save dropped the reference for
good. `fps_shooter` lost three models that way, with nothing in the log.

`unresolved_mesh.h`: the disk loader stores the entity's `meshRenderer` object
verbatim in `UnresolvedMesh` before it tries anything, and removes it only when
a `MeshRenderer` is set. `saveMesh` writes it back unchanged (disk) or under
`"unresolved"` (memory snapshots: undo and Snapshot Play), so stopping Play
cannot erase it either. A `MeshRenderer` always wins. The Inspector shows
"Mesh missing: <path>" and the reason instead of an empty-looking entity, and a
failed async import now logs instead of returning in silence.

Not covered: the cooked binary path (`SceneService`) is read-only and cannot
lose data, but it still fails a mesh load silently.

## A scene must outlive the kit that authored it — now tested

`ReflectedPending` stashes a component blob whose type is not registered and
re-emits it on save, so a scene round-trips losslessly through a session whose
kits are missing. The design was there; **the save was never tested**, and the
save is the only step that can destroy anything — a load that stashes correctly
and a save that omits the stash look identical from inside the session doing the
damage, which is how a colleague's file gets ruined by someone who never saw a
warning.

`tests/reflected_pending_test.cpp` runs every case THROUGH a kit-less session and
saves from it: both unknown components re-emitted (not just the first), the result
byte-identical to what the kit wrote, five consecutive kit-less saves changing
nothing, a half-equipped session that has one of two kits still writing both, and
`applyPending` restoring exactly the original values — asserted with `==` on
floats, because "close enough" is how a document drifts every time it passes
through a machine missing a plugin. Mutation-proved: deleting the re-emission from
`save()` fails four assertions.

Hermetic — "the type is not registered" is the whole condition, so it needs no
kit, no dlopen and no project. Worth knowing from writing it: flecs emits floats
with 10 significant digits, so the JSON round-trip is exact rather than nearly so.
