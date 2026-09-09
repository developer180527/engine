---
status: as-built
tier: hardened
verified: 2026-09-09
covers:
  - src/components/
tests:
  - tests/component_abi_test.cpp
  - tests/camera_look_test.cpp
  - tests/determinism_gate_test.cpp
  - tests/event_test.cpp
  - tests/sim_purity_check.cpp
  - tests/stress_churn.cpp
---
# Components

## The layout is an ABI, and it is frozen

These structs are not private to the engine. Thirteen of them are hashed into
`engine_abi::componentLayoutHash()`, which `ModuleLibrary::load` uses to decide
whether a kit may touch **live ECS memory** — and whose refusal says *restart the
host*, not *rebuild the module*, because world data survives a reload.

**That hash covers `sizeof` and `alignof` only.** Reordering two same-sized
fields changes neither, so the gate accepts a kit built against the old order and
that kit then reads and writes the wrong field, every frame, in memory the host
owns. Demonstrated rather than argued — swapping `CharacterController::radius`
with `::height`:

```
componentLayoutHash()  before: e369fed4bf52e60f
                        after: e369fed4bf52e60f     ← identical
```

`tests/component_abi_test.cpp` closes that. It is the same treatment
`api_abi_compat_test` gave the API table for the same reason
([`extension-model.md`](../../docs/architecture/extension-model.md) §1.3: *"a
reordered group keeps every size intact and still breaks every Kit"*), and until
2026-08-29 `offsetof` appeared in exactly one file in this tree. Components are
the more exposed of the two surfaces: a kit **writes** them.

Pure-POD components get exact `sizeof` and exact offsets. The four containing
`std::string`/`std::vector` get **field order** instead, because their sizes
differ between standard libraries and freezing a byte count would fail on Linux
for a reason that is not a defect — cross-toolchain mixing is already refused by
`abiFingerprint`.

> **Changing a component layout is always a deliberate act.** Update the frozen
> numbers in the same commit, and expect `componentLayoutHash()` to refuse every
> older module — which is the gate working, not the gate breaking.

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

## Every component carries a classification

`sim_state.h` adds a second frozen property alongside the layout ABI above:
**is this component simulation state?** Its type carries exactly one of
`SimState` or `SimExempt`, registered in `runtime/sim_classification.cpp`.

It exists because a simulation that is not reproducible cannot have behavioural
regression tests. `tests/determinism_gate_test.cpp` hashes classified component
state after every tick and compares runs; the governing rule for what to hash is
**every field that can influence future simulation behaviour** — not every
field, and not the fields that happen to be serialized. `Animator::fade` is in
(it is a crossfade duration and changes the pose sampled on later ticks);
`Camera` is out entirely.

`CameraLook` (2026-09-09) is out for a sharper reason than the rest, and it is
the reason the component exists. A first-person controller must latch the mouse
at RENDER rate or look lags the frame rate — but it then wrote the resulting
orientation into `Transform.rotation`, which IS hashed. That is a render-rate
write to simulation state: BUG-0053's defect class, alive in the tree only
because no gate tier drives input, so nothing measures it. Moving the aim into
its own `SimExempt` component is what makes the latch legal instead of merely
unmeasured. `camera_look_test.cpp` pins both directions — twelve render-rate aim
changes leave the world hash untouched, and writing the same aim the old way
still changes it. **It does not make look deterministic**: yaw and pitch still
accumulate per frame and still feed movement and raycasts, which are simulation.
Closing that needs the input layer named in `runtime/sim_command.h`.

**Adding a component means classifying it.** `simhash::auditCoverage()` fails on
any data component present in a world carrying neither tag, so the default is
not "silently uncovered" but "the gate tells you to classify it". Every
exemption carries a written reason and the registry refuses one without it — an
exemption with no reason is indistinguishable from an oversight.

This is deliberately NOT the same question as `SerdeTransient`, and the two
disagree in both directions: `Light.intensity` is serialized and is not
simulation state; `CollisionEvents` is `SerdeTransient` and IS simulation state.
Reflection asks what the inspector and the scene format can see; classification
asks what a divergence would be observable in.

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
