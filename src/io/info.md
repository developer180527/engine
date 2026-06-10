# IO

## Purpose
Everything that crosses the process boundary: project metadata, asset
importers (source formats), scene serialization (JSON + cooked binary), and
the background cook service.

## Architecture
- **`ProjectContext`** (`project_context.h`) — `project.json` (name, asset
  root, last scene). `autoDetect()` walks up from cwd; `load(root)` is the
  explicit form. The runtime loads it at init.
- **Importers** — `IMeshImporter` implementations behind `ImporterRegistry`
  (extension → importer):
  - `GltfImporter` (cgltf) — glTF/GLB.
  - `AssimpImporter` — FBX, OBJ, COLLADA, 3DS, PLY, STL, Blend. FBX uses
    `PRESERVE_PIVOTS=false` (see `src/animation/info.md` for the precision
    consequences).
- **Scene serialization** (`scene_serializer.h`, `entity_serializer.h`) —
  JSON `.scene` files; component-by-component (de)serialization via
  `AssetStorage` (bundle of all content registries). Two load paths:
  - `loadAsync` — JSON parsed immediately (names + transforms synchronous),
    meshes/textures stream in via `AsyncLoader`.
  - `cookScene`/binary — scene cooked to `.cache/scenes/*.cooked` for the
    fast `SceneService` path.
- **`CookService`** (`cook_service.h/.cpp`) — drives `assetlib::CookPipeline`
  with the mesh/texture cookers. Editor runs it on a background thread
  (`start()`, WAL-mode SQLite allows concurrent main-thread reads);
  `cookOnce()` is the synchronous CLI path (`engine_cook`).

## Data Flow
```
source asset (fbx/gltf/png...) ── scan → registry.db (UUID, hash, state)
        │                                     │
        │ (editor drag-in / scene load)       │ CookService (stale check)
        ▼                                     ▼
  Importer → CPU mesh data              MeshCooker/TextureCooker
        ▼                                     ▼
  GPU upload (main thread)             .cache/*.cooked (binary fast path)
```
Cooked binaries load via `src/loaders/mesh_loader.cpp` without touching
Assimp. Skinned meshes currently always take the importer path (cooker
returns `skipped`).

## Invariants
- Registry DB at `<project>/.cache/registry.db`, SQLite WAL. One writer
  (cook thread), many readers.
- Cook failure must delete stale cooked output (no half-written binaries).
- Scene saves auto-cook the JSON to binary so both formats stay in sync.

## Future Work
- Custom asset type registration (cooker + loader pairs from plugins).
- Fix stale `Animator.clip` handle serialization in `loadAsync` callback.
