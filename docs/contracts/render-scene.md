---
status: as-built
contract: render-scene
kind: interface
state: provisional
owner: src/render/world
header: src/render/world/render_scene.h
implementations:
  - real: src/render/world/render_scene.h#RenderTable
  - null: src/render/world/render_scene.h#NullRenderScene
tests:
  - tests/render_scene_test.cpp
covers:
  - src/render/world/render_scene.h
  - src/render/world/render_scene.cpp
verified: 2026-10-01
---

# render-scene — the retained scene's rows

`create / update / destroy → RenderObjectId`: one row per drawable object, the
table the renderer will remember instead of rebuilding every frame
(`docs/plans/renderer-program.md` §9). P3a (WO-019) built it and proves it; the
renderer writes it wholesale from extraction (`RenderScene::apply`) when
`ENGINE_RENDER_TABLE=1`, and nothing reads it until P3c.

## Nothing
`NullRenderScene`, for a build with no renderer: `create` returns an invalid id
(`!id.valid()`), and `update`/`destroy` on any id are no-ops returning false. A
caller never branches on "is there a renderer". An invalid or stale id is
treated the same way by the real table: refused, never aliased.

## Ownership
The table owns every row; an id is a value (`{index, generation}`), never a
pointer, and is freely copied. Rows hold integers and floats only (handles,
not pointers), so nothing a row names is owned by it. `RenderScene` (one per
world) owns its two tables, and the renderer owns one `RenderScene` per world
it draws, dropping the play world's with that world.

## Threading
Single-threaded: the renderer calls it from the main thread at the apply
point, after the parallel extraction has finished writing its capture. No
call blocks; nothing in it takes a lock.

## Timing
Once per frame per world, before the first view of that frame (the single
apply point, §9.3). `destroy` moves a row to RETIRED on the frame it happens;
the slot returns to FREE only when `collect` is given a completed frame at or
after it, which the renderer does `RenderScene::kFramesInFlight` frames later.
A generation bump at retire makes the old id invalid at once.

## Errors
No exceptions and no logging: `update`/`destroy` return false for an id that
is not LIVE (stale, retired, or invalid), and `read` returns false. The
rebuild-and-diff check (`RenderScene::diff`, `ENGINE_RENDER_TABLE_DIFF=1`)
returns the first mismatch naming the entity and the column; the renderer
logs it and aborts, since it is a debug mode and the first divergence is the
finding.
