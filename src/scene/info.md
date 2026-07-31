---
status: unreviewed
tier: prototype
covers:
  - src/scene/
---
# Scene

## Purpose
World (de)serialization — the single component serde path that scene save/
load, the play-mode snapshot, and the undo stack all share. Future home of
prefabs.

## Contents
- **`entity_serializer.h`** — the one component serde table. Two modes:
  `Disk` (cross-session: AssetRef uuid+relative refs, semantic clipIndex —
  session handles NEVER hit disk) and `Memory` (same-session snapshot: live
  handle reuse). Add a component once here and every path picks it up.
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
