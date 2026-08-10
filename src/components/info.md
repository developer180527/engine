---
status: as-built
tier: working
verified: 2026-08-10
covers:
  - src/components/
tests:
  - tests/event_test.cpp
  - tests/sim_purity_check.cpp
  - tests/stress_churn.cpp
---
# Components

## Purpose
Plain-data ECS component definitions shared by runtime, plugins, and editor.
No logic beyond trivial helpers — systems and plugins own behavior.

## Contents
- `name.h` — display/lookup name.
- `mesh_renderer.h` — `MeshHandle` + material override.
- `skinned_mesh.h` — skeleton handle + a palette SLOT (`paletteSlot`,
  `hasSkinMatrices`). The `mat4[128]` palette itself lives in
  `anim::skinPalettes()`, not the component: inline it made the component
  8 200 bytes, and the renderer's extraction query reads five of those bytes per
  entity while paying the whole thing as stride. Resolve with
  `skinPalettes().at(paletteSlot)`, which returns null for `kNoSlot` so an
  unanimated entity needs no special case.
- `lod_mesh.h` — the COARSER levels of a mesh plus their screen-height
  thresholds. Level 0 is `mesh_renderer.h`'s own mesh, so no chain (or
  `count == 0`) means full detail; the renderer selects a level at extraction
  (`render/world/lod.h`, `render/issues.md` R20).
- `animator.h` — clip handle, time, speed, playing/looping flags.
- `camera.h`, `light.h` — render inputs (game view picks the primary camera).
- `rigid_body.h`, `character_controller.h`, `collision_events.h` — physics
  (consumed by JoltPlugin).
- `script_component.h` — Lua script path (consumed by LuaScriptPlugin).
- `spinner.h` — demo component (default scene cubes).
- `entity_id.h` — stable serialization identity.
- `serde_transient.h` — `SerdeTransient` type tag: runtime state, never saved.
- `event_component.h` — the EVENT model. `events::declare<T>(world)` marks a
  component type a one-shot message (implies SerdeTransient) that the runtime's
  `EventSweeper` (runtime/event_sweeper.h) keeps observable for a full tick
  after it's written, regardless of plugin broadcast order, then auto-expires.
  Drain a handled event with `events::consume<T>(entity)` — never a bare
  `remove<T>()` (leaves an orphaned `(EventStale, T)` staleness marker). Events
  are MESSAGES; persistent-within-session markers (e.g. combat::Died) stay
  plain SerdeTransient STATE, cleared by their owner.

## Rules
- Components must stay POD-ish and serializable: every field either round-
  trips through the scene serializer (`src/scene/entity_serializer.h`) or is
  explicitly runtime-only (like `skinMatrices`).
- New components need: serializer support, MetaRegistry schema (inspector +
  Lua FFI), and inspector UI if user-editable.
- **Serialization checklist — a component is enumerated in THREE places**
  (known duplication; unification is future work):
    1. `scene/entity_serializer.h` (JSON save/load table)
    2. `assets/cookers/scene_cooker.cpp` (JSON → binary)
    3. `runtime/services/scene_service.cpp` (binary → entities)
  Touching only the first makes the component silently vanish from cooked
  scenes. Also: changing any component's LAYOUT invalidates hot-reload
  modules (engine_abi::componentLayoutHash) — restart engine_host.
- Asset references serialize as registry ids/UUIDs, never raw `Handle`
  values (handles are session-local).
