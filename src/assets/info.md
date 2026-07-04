# Assets

## Purpose
The asset pipeline, end to end: stable identity, source-format import,
cooking to engine-native binaries, and loading those binaries back. Folder
structure mirrors the stages — and the library layering:

```
assets/
├── asset_ref.h            identity (engine_core-safe headers)
├── importers/             source formats → engine data   [engine_runtime*]
├── cookers/               source → .cache binaries        [engine_core]
└── loaders/               cooked binaries → GPU           [engine_runtime]
```
(*`stb_impl.cpp`/`cgltf_impl.cpp` are CPU-only implementation TUs and belong
to engine_core; the importer .cpps create GPU resources and are runtime.)

## Identity — AssetRef (`asset_ref.h`)
THE way scenes reference assets on disk:
`{"asset": "<uuid>", "path": "assets/models/mask.glb"}`.
UUID (assetlib) is the identity — survives sessions, machines, renames and
moves (the registry re-matches moved files by content hash at scan time).
The path is project-relative, never absolute. Resolution: uuid → registry →
current path; then relative path; legacy absolute paths tolerated read-only.
Session handles (registry slot indices) must NEVER be serialized.
`tools/scene_resave.cpp` migrates legacy scenes.

## Importers (`importers/`)
`IMeshImporter` implementations behind `ImporterRegistry` (extension →
importer): `GltfImporter` (cgltf) for glTF/GLB; `AssimpImporter` for FBX,
OBJ, COLLADA, 3DS, PLY, STL, Blend. FBX uses `PRESERVE_PIVOTS=false`
(see `src/animation/info.md` for the precision consequences).

Both importers MERGE every submesh/primitive of a source file into ONE `Mesh`
— a shared VB/IB (base-vertex-offset indices) with a `SubmeshRange` per source
part carrying its own material — the representation the cooker + renderer
already use. (Historically glTF read only `meshes[0].primitives[0]` and Assimp
returned only the first submesh, silently dropping the rest — see the resolved
`importers/issues.md`.) A single-submesh model stays on the simple single-draw
path (mesh.material set, `submeshes` empty). The whole model is skinned iff it
has a skeleton (one merged vertex format); a bone-less submesh inside a skinned
model binds rigidly to bone 0 so it can't collapse. Verified headless by
`tools/import_test.cpp` (bgfx Noop backend).

## Cookers (`cookers/`)
`assetlib::ICooker` implementations + `CookService`:
- `MeshCooker` — imports via Assimp directly, writes vertex/index buffers +
  submeshes + bounds. Returns `skipped` for skinned meshes (no cooked format
  for bone data yet).
- `TextureCooker` — stb decode → GPU-ready texels.
- `SceneCooker` — scene JSON → binary for SceneService.
- `CookService` — drives the pipeline: background thread in the editor
  (`start()`, WAL SQLite allows concurrent main-thread reads), synchronous
  `cookOnce()` for the engine_cook CLI.

Rules: cookers may use Assimp/stb/assetlib but must never reference bgfx,
GLFW, or plugin symbols — `engine_cook` links engine_core alone. Staleness is
content-hash based; failed cooks must delete stale output; cooked formats
carry versioned headers.

## Loaders (`loaders/`)
`mesh_loader` — reads a `.cooked` mesh and creates bgfx buffers. The fast
path that skips importers entirely. Validate the header version; mismatch
means "treat as missing" and fall back to import.

## Data Flow
```
source asset ── scan → registry.db (UUID, hash, state)
      │                      │ CookService (stale check)
      ▼                      ▼
  importers/  ←fallback─  cookers/ → .cache/*.cooked → loaders/ → GPU
```

## Future Work
- Skinned-mesh cooking (bone data + skeleton + clips in one binary).
- Custom asset type registration (cooker + loader pairs from plugins).
