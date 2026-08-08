---
status: as-built
tier: hardened
verified: 2026-08-08
parses-external-input: true
covers:
  - src/scene/
tests:
  - tests/kit_lifecycle_test.cpp
  - tests/fuzz_entity_serde_test.cpp  # the JSON deserializer, hostile input
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
a missing, wrong-typed, short, or non-finite value.

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
- **`scene_serializer.h`** — `.scene` JSON save/load on top of the table:
  - `loadAsync` — names/transforms synchronous, meshes stream via
    AsyncLoader; the import callback resolves skeleton/clip handles fresh
    (Animator::clipIndex selects the clip).
  - `saveToString`/`loadIntoWorld` — instant play-mode snapshot.
  - `cookScene` — thin wrapper over `assets/cookers/scene_cooker`.

## Invariants
- Caller runs `assignMissingIds()` before save (set<> is illegal mid-query).
- Parent links restore in a post-pass (all entities must exist first).
- Scene saves auto-cook the binary twin (editor keeps both in sync).
