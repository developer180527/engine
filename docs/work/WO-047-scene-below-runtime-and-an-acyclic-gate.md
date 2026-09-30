---
status: plan
id: WO-047
title: Scene serialization stops reaching into runtime services; the module graph is gated acyclic
program: process
priority: P2
size: M
state: done
done: 2026-10-01
evidence: audit LAYER-03 drops scene -> runtime (47 -> 46 edges) and LAYER-06 reports no cycle (mutation: restoring the include fails --check, naming the cycle and its files); scene_material_roundtrip_test (BUG-0072, both halves mutation-checked); 136 tests, audit and doctor clean
depends: [WO-046]
touches:
  - src/scene/entity_serializer.h
  - src/scene/scene_serializer.h
  - scripts/engine_audit.py
  - src/scene/scene_assets.h
  - src/runtime/services/scene_assets_host.h
source: architecture audit 2026-09-30
---
## Why
The last back-edge in the module cycle (WO-046 has the other four) is `scene <-> runtime`.

`runtime` uses `scene` to load and snapshot worlds, which is the right direction. But the serializers reach back UP into runtime services: `entity_serializer.h` includes `runtime/services/asset_service.h`, and `scene_serializer.h` includes `async_loader.h` and `asset_service.h`. Loading a scene resolves its assets by calling the services directly.

Once WO-046 and this are done the graph is a hierarchy. Then it should STAY one, which today nothing checks.

## Done when
- [x] `src/scene` includes nothing from `src/runtime`: the serializers take what they need to resolve assets as a narrow interface or callbacks the runtime supplies (the host resolves, the serializer describes), and the `scene -> runtime` edge leaves the baseline
- [x] a new audit rule, LAYER-06, fails if the module graph has a cycle, naming the cycle and the files that create each edge in it
- [x] `docs/process/architecture-audit.md` lists LAYER-06 and states the layer order the graph now has

## Contract
Changes how scene loading reaches assets: through what the runtime hands the serializer, not by including the runtime.

Nothing: a scene loads the same entities with the same assets; the runtime supplies the resolver it already owns.

## Log
- 2026-10-01:
  - **The hooks.** `src/scene/scene_assets.h` declares `SceneAssets`: four
    optional `std::function`s (load a cooked mesh and its LOD levels, load a
    material by name, name a material handle, stream a source mesh) and
    `StreamedMesh`, what a stream produced. It includes only `core/handle.h`.
    `SerdeContext::assetService` became `const SceneAssets* sceneAssets`;
    `loadAsync` takes `const SceneAssets&` in place of `AsyncLoader&` and
    `AssetService*`; `save` takes an optional `const SceneAssets*`. The
    runtime builds one in `runtime/services/scene_assets_host.h`
    (`sceneAssetsFor(AssetService*, AsyncLoader*)`), and the editor,
    `engine_host` and `scene_resave` call it.
  - **Cost, as asked.** The hooks run once per asset reference while a scene
    loads or saves, around work measured in milliseconds; no tick reaches
    them. `sim_profile`'s 600-tick runs never showed a serializer scope, and
    the play-mode snapshot (`saveToString`/`loadIntoWorld`) is Memory mode,
    which uses no hook at all.
  - **BUG-0072, found by the move.** `save` installed the material-name
    lookup only `if (ctx.assetService)`, on a context where nothing set that
    field, so no material override was ever written (since 0f15c73, the
    commit that introduced names). And a mesh streamed on a worker dropped
    its override at load: the callback set `MeshRenderer{mesh}`.
    `PendingMesh` now carries the resolved material. Pinned by
    `scene_material_roundtrip_test`, which runs the real `save`/`loadAsync`
    with a fake `SceneAssets`.
  - **Behaviour kept, one addition.** A host with no streamer (none today:
    all three callers pass a loader) keeps the mesh reference in
    `UnresolvedMesh` with the reason "no loader", as the no-importer path
    already did.
  - **LAYER-06.** Tarjan over `module_edges()`; each strongly connected
    component of more than one module is a finding naming the cycle and up
    to four files per edge. It is the one rule with `baselinable=False`:
    `--update-baseline` does not record it, and a hand-added entry is
    ignored. Checked by putting the include back: `--check` exits 1 with
    `cycle src/runtime <-> src/scene` and the files on both edges.
  - **The layer order** is in `docs/process/architecture-audit.md` §4:
    core/project, then animation/audio/components, render/systems, assets,
    scene, runtime, plugins, editor/tools.
