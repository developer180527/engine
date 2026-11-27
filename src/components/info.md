---
status: as-built
tier: hardened
verified: 2026-11-28
covers:
  - src/components/
tests:
  - tests/component_abi_test.cpp
  - tests/camera_look_test.cpp
  - tests/determinism_gate_test.cpp
  - tests/event_test.cpp
  - tests/sim_purity_check.cpp
  - tests/stress_churn.cpp
  - tests/change_stream_test.cpp   # the change stream (WO-072)
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

**The meaning is part of the ABI too (WO-051).** Each of the thirteen declares
`static constexpr uint32_t kAbiRevision`, folded into the hash when it is not 0.
Raise it when the same bytes come to mean something else, as WO-048 did to
`CollisionEvents` (revision 1: kept with empty lists rather than removed). The
loader then refuses older kits by naming the component, and
[`kit-abi-revisions.md`](../../docs/guides/kit-abi-revisions.md) says what changed.
`kit_abi_headers` (ctest) fails on any edit to these headers until it is recorded
as comment-only or as a revision bump (`scripts/kit_abi_headers.py`).

## Purpose
Plain-data ECS component definitions shared by runtime, plugins, and editor.
Behaviour belongs to systems and plugins, not here.

The exceptions are the few headers that are **ECS glue for these components** —
`meta_registry.h`, `event_component.h`, and since 2026-09-16 `entity_id_util.h`
and `transform_hierarchy.h`. They operate on the structs next door and need
`<flecs.h>`, which `src/core` may not include, so this is where they live. None
of them is part of the frozen kit ABI and none is in `componentLayoutHash()`:
the ABI is the plain-data structs listed below, not the helpers beside them.

## Contents
- `name.h` — display/lookup name.
- `mesh_renderer.h` — `MeshHandle` + material override.
- `skinned_mesh.h` — skeleton handle + a palette SLOT (`paletteSlot`,
  `hasSkinMatrices`). The `mat4[kMaxBones]` palette (128, from `core/bone_limit.h`, WO-040) lives in
  `anim::skinPalettes()`, not the component: inline it made the component
  8 200 bytes, and the renderer's extraction query reads five of those bytes per
  entity while paying the whole thing as stride. Resolve with
  `skinPalettes().at(paletteSlot)`, which returns null for `kNoSlot` so an
  unanimated entity needs no special case.
- `lod_mesh.h` — the COARSER levels of a mesh plus their screen-height
  thresholds. Level 0 is `mesh_renderer.h`'s own mesh, so no chain (or
  `count == 0`) means full detail; the renderer selects a level at extraction
  (`render/world/lod.h`, `render/issues.md` R20). Sized by
  `core/lod_limit.h`, so this header includes nothing from render (WO-046).
- `animator.h` — clip handle, time, speed, playing/looping flags.
- `camera.h`, `light.h` — render inputs (game view picks the primary camera).
- `rigid_body.h`, `character_controller.h`, `collision_events.h` — physics. `CollisionEvents` holds THIS tick's contact starts and ends; a body keeps the component once it has had a contact, and a quiet tick is two empty lists, never a missing component (WO-048)
  (consumed by JoltPlugin).
- `script_component.h` — Lua script path (consumed by LuaScriptPlugin).
- `spinner.h` — a constant turn (radians per second about local Y, then X).
  Saved like any component since 2026-11-28 (`core.Spinner`, key `spinner`).
  It used to double as the demo grid's "never saved, not listed" marker, which
  dropped a user's spinning entity from the file on save and meant a scene's
  `spinner` never loaded (the fuzz scene's movers never moved).
- `demo_grid.h` — `DemoGrid`, the tag on the default scene's 3x3 cube grid:
  made by code each boot with a primitive mesh, so never saved, never copied
  into a Play snapshot, not listed in the hierarchy, kept by Clear Scene.
- `entity_id.h` — stable serialization identity.
- `entity_id_util.h` — the helpers over `EntityId`: `generateEntityId`,
  `ensureEntityId`, `assignMissingIds`, and `EntityIdIndex`. Loaders build the
  index once per load and ask it; `findById` is O(n) and is for ONE-OFF lookups
  (undo, an editor click) — calling it per entity is what made scene load
  quadratic (97.6% of a 50 000-object load's 22 s, runtime `issues.md` H.0b).
  Moved here from `src/core` on 2026-09-16: it is entirely flecs, and core may
  not include the ECS.
- `transform_hierarchy.h` — world poses through the `ChildOf` chain:
  `getWorldMatrix`, `getWorldMatrixLerp` / `...From`, `safeReparent` (refuses
  cycles, and chains deeper than the limit flecs *aborts* the process on),
  `hierarchyDepth`, `isAncestorOf`, and (since WO-046) `localMatrixLerp`,
  which takes a `PrevTransform`. Split from `core/transform_utils.h` on
  2026-09-16 for the same reason; the matrix math it calls stayed in core. It
  lives here rather than in `scene/` because every caller — renderer extraction,
  the Jolt plugin, `script_host`, the editor, the serializer — already depends on
  `components/`, so this home adds no new module edge.
- `world_query_cache.h` — per-world cached flecs queries for systems that
  run over the editor and play-mode worlds alike (the renderer's extraction,
  the animator). Moved from `src/runtime/` (WO-046): it is ECS infrastructure,
  and render and systems including it from runtime made both depend on the
  orchestration layer.
- `change_stream.h` — what changed in a component since you last looked
  (WO-072). A provider that keeps its own copy of the world subscribes to a
  component (or a tag, or a pair such as `(ChildOf, *)`), reads ADDED /
  CHANGED / REMOVED per entity, and unsubscribes; the C world API (WO-073) is
  a thin layer over it. Nothing exists until something subscribes: an empty
  game creates no stream and no observer (`empty_game_budget_test`).
  Structural changes, `set<T>()` and `modified<T>()` are seen by an observer;
  a write IN PLACE is declared by the writer with `changes::Marks<T>`
  (`column(it)` for every row of a batch, `rows(it, first, count)` for the
  rows it wrote, `entity(e)` for one) or `changes::markChanged<T>(e)`. Merged
  per read: an entity appears once between two reads. Subscribing queues
  everything that already has the component as ADDED. Held in the world's
  context pointer (`ecs_set_ctx`), which nothing else uses.
- `prev_transform.h` — the interpolation history, and `samePose(t, p)`
  (bitwise). A pose equal to its history is not interpolated
  (`localMatrixLerp`), so a resting object's matrix does not move by an ulp
  as alpha changes.
- `serde_transient.h` — `SerdeTransient` type tag: runtime state, never saved.
- `event_component.h` — the EVENT model. `events::declare<T>(world)` marks a
  component type a one-shot message (implies SerdeTransient) that the runtime's
  `EventSweeper` (runtime/event_sweeper.h) keeps observable for a full tick
  after it's written, regardless of plugin broadcast order, then auto-expires.
  Drain a handled event with `events::consume<T>(entity)` — never a bare
  `remove<T>()` (leaves an orphaned `(EventStale, T)` staleness marker). Events
  are MESSAGES; persistent-within-session markers (e.g. combat::Died) stay
  plain SerdeTransient STATE, cleared by their owner.
- `colour_grading.h` — exposure (Manual gain or Physical aperture/shutter/ISO),
  tone mapper and grading-LUT path for a camera (colour pipeline stage B). A
  component BESIDE `Camera`, not fields in it: `Camera` is in
  `componentLayoutHash`, and this is presentation no kit reads — not re-exported,
  not hashed, `SimExempt`. Enums are stored as bytes; `resolvedExposure` and
  `resolvedToneMapper` refuse NaN, overflow and unknown ids at use rather than at
  load, so a newer engine's scene round-trips. Meta is registered by
  POINTER-TO-MEMBER so the inspector's offsets come from the real layout —
  `colour_grading_test` §3 reads every field back through flecs.

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
- **A write in place must be declared** (WO-072). Writing through a mutable
  query column, `get_mut` or `try_get_mut` is invisible to the change stream;
  mark it with `changes::Marks<T>` or `changes::markChanged<T>(e)`, or use
  `set<T>()`. Over-reporting is allowed, missing a write is the bug, and
  `ENGINE_RENDER_TABLE_DIFF=1` names the entity and column of a write that
  was not marked. Mark on the world's own thread: a parallel writer collects
  what it wrote and marks after the join (the animator does).
- Asset references serialize as registry ids/UUIDs, never raw `Handle`
  values (handles are session-local).
