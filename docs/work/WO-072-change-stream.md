---
status: plan
id: WO-072
title: One change stream per registered component — free without subscribers, merged per read
program: renderer
priority: P1
rank: 4
size: M
state: done
done: 2026-11-28
evidence: CI run 46061007680 (6760c49) green on the full matrix, sanitizers included; change_stream_test (38 checks) mutation-checked four ways; diff mode green on the 50 000-object fuzz scene for 900 frames with its spinners turning and 1 500 with 12 506 falling bodies
depends: [WO-071]
contracts: [render-scene]
touches:
  - src/components/change_stream.h
  - src/render/retained_input.h
  - src/render/world/render_scene.h
  - src/render/renderer/extract.cpp
  - src/components/demo_grid.h
  - src/scene/entity_serializer.h
source: docs/plans/engine-architecture.md §5; engine-api.md §5 (marked writes, merged changes); roadmap §2.8
---
## Why
Every provider that keeps its own copy of the world (the renderer's GPU scene, physics bodies, audio voices, replication) needs to know what changed since it last looked. WO-019 built this for the renderer alone, as a rebuild-and-diff table; every provider moving onto the C API needs it general.

## Done when
- [x] subscribe, read and unsubscribe per registered component type (the draft's `engSubscribeChanges`, `engReadChanges`), in core (2026-11-28: `components/change_stream.h`, any component, tag or pair; the C names are WO-073's thin layer over it, which its done-when already lists)
- [x] **free with no subscriber**: an empty game records nothing, checked by `empty_game_budget_test` (DR-0011) (2026-11-28: no stream object exists in an empty game's world; a writer's check is one null load)
- [x] merged per read: an entity appears once between two reads (changed many times is one CHANGED, added then removed is nothing), and an id is not reused while a stream could still report it (2026-11-28: `change_stream_test` §2–3; ids carry flecs' generation, so a recycled slot is a new id; the stated limit is its 16 bits)
- [x] declared writes: a writing system marks a whole column, or with `ENG_ACCESS_WRITE_MARKED` only the rows it reports; a test shows sleeping physics bodies do not appear when only awake ones are marked (2026-11-28: `changes::Marks<T>` column/rows/entity; `change_stream_test` §4 and §6, a real runtime with Jolt)
- [x] the retained render scene (WO-019) reads it instead of its own capture, and its rebuild-and-diff check still passes on the fuzz scene (2026-11-28: green for 900 frames on the fuzz scene and 1 500 on a copy with falling bodies; see the log)
- [x] mutations: a write that does not mark, and a dropped REMOVED, are each caught by a test (2026-11-28: four mutations, each red; see the log)

## Contract
Nothing: `render-scene`'s rebuild-and-diff mode stays as the oracle; a provider that does not subscribe sees no change.

## Log
- 2026-11-28 — **Built.**
  - **The stream** (`components/change_stream.h`). Per world, held in the
    world's context pointer, created at the first subscribe. A subscription
    to an id (component, tag, or pair such as `(ChildOf, *)`) starts an
    observer for OnAdd, OnRemove and, for an id with data, OnSet, matching
    prefabs and disabled entities too; the last unsubscribe deletes it.
    Subscribing queues every entity that already has the id as ADDED, so a
    provider builds its copy from the stream alone. Merged per read by an
    entity-to-entry index; reads return oldest first, a batch at a time.
  - **Declared writes.** `changes::Marks<T>` is made once per pass and is off
    when nothing subscribes: `column(it)` (the draft's ENG_ACCESS_WRITE),
    `rows(it, first, count)` (ENG_ACCESS_WRITE_MARKED + engMarkChanged),
    `entity(e)`. The engine's in-place writers mark: prevSnapshot (only
    rows whose history moves), spinners, teleport, `script_host`'s teleport
    and material, Jolt's two write-backs (only poses that moved), the
    animator (palette slot or skinned flag changed, marked after the
    parallel step), and the editor's gizmo, Transform inspector, undo,
    reparent and material override.
  - **A resting object no longer moves by an ulp.** `bx::lerp(a, a, t)` is
    `t*a + (a - t*a)`, not exactly `a`, so a resting object's interpolated
    matrix changed with alpha every frame. `localMatrixLerp` now skips the
    lerp when the pose equals its history (`samePose`, bitwise). Without it,
    every row with a PrevTransform would have been "changed" every frame.
  - **The retained scene reads it** (P3b). `render/retained_input.h` decides
    which rows to write: reported owners, rows still interpolating, and
    parented rows (their matrix depends on ancestors). A full rewrite happens
    on the first frame, on a mesh or skeleton registry change (new
    `revision()` counters), and when interpolation switches on or off.
    `RenderScene::applyChanged` writes the dirty rows and retires reported
    owners with no row. Diff mode captures every row, writes only the dirty
    ones, and compares against a rebuild.
  - **Tests.** `change_stream_test` (38 checks): free, merge rules, partial
    reads, two streams, deletion and recycling, declared writes, tags and
    pairs, a real runtime with Jolt (8 sleeping boxes not reported; a falling
    body and a spinner are), and the retained scene written incrementally
    against its rebuild (an unmarked write named by entity and column).
    `empty_game_budget_test` checks no stream exists in an empty game.
  - **Mutations, each red, then restored:**

    | mutation | caught by |
    |---|---|
    | OnRemove ignored (a dropped REMOVED) | 9 checks: merge rules, recycling, Disabled, reparenting, the retained scene leaking a row |
    | Jolt's write-back not marked | §6: the falling body not reported |
    | prevSnapshot not marked | §6: PrevTransform reports nothing |
    | spinners not marked | §6: the spinner not reported |

  - **On the fuzz scene** (`gen_fuzz_scene --objects 50000 --seed 1`,
    `build-prof`, windowed `engine_host`). **Finding:** its 12 506 movers do
    not move, because their `"spinner"` key has had no schema since WO-071,
    so they load as kept unknown data. The diff stayed green for 900 frames,
    writing 7 550 rows a frame (the parented ones). For motion, a copy whose
    12 506 movers are dynamic rigid bodies dropped from 100–500 m onto a
    floor: green for all 1 500 frames, from ~9 600 moving rows a frame to
    7 556 once the bodies slept.

    | | `Render.extract` | `Render.table` |
    |---|---|---|
    | table off, static | 4.1–4.3 ms | — |
    | table on, static | 4.7–4.8 ms | 0.13 ms (WO-019: 0.46) |
    | table on, bodies falling | 5.3–5.9 ms | 0.13–0.30 ms |
    | diff mode, bodies falling | 10.0–10.8 ms | 4.7–5.0 ms |

    Extraction still computes every item, so the table costs what it did to
    have on; P3c (extraction reads the table) is what removes that.
  - **The movers, fixed the same day.** Spinner doubled as the demo grid's
    "never saved, not listed" marker. So a scene's `"spinner"` had no reader,
    and a user's entity given a Spinner in the inspector vanished from the
    file on save. The grid now has its own tag (`components/demo_grid.h`),
    and Spinner is a saved schema (`core.Spinner`). The fuzz scene's movers
    turn: diff mode green for 900 frames at 18 184 rows written a frame
    (table on: `Render.table` 0.21 ms, `Render.extract` 4.2 ms off and 5.8 ms
    on). Registering a component changes every packed file's schema table,
    so the packed portability golden was re-cooked; the old one is kept as
    `every_component.before_spinner.packed.cooked` and must still read.
  - Unit lane 137/137; full ctest green but for the docs lane, then the docs
    lane green after the docs were updated.

