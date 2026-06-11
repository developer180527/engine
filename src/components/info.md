# Components

## Purpose
Plain-data ECS component definitions shared by runtime, plugins, and editor.
No logic beyond trivial helpers — systems and plugins own behavior.

## Contents
- `name.h` — display/lookup name.
- `mesh_renderer.h` — `MeshHandle` + material override.
- `skinned_mesh.h` — skeleton handle + bone palette
  (`skinMatrices[128*16]`, `hasSkinMatrices`).
- `animator.h` — clip handle, time, speed, playing/looping flags.
- `camera.h`, `light.h` — render inputs (game view picks the primary camera).
- `rigid_body.h`, `character_controller.h`, `collision_events.h` — physics
  (consumed by JoltPlugin).
- `script_component.h` — Lua script path (consumed by LuaScriptPlugin).
- `spinner.h` — demo component (default scene cubes).
- `entity_id.h` — stable serialization identity.

## Rules
- Components must stay POD-ish and serializable: every field either round-
  trips through the scene serializer (`src/io/entity_serializer.h`) or is
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
