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
GLFW, or plugin symbols — `engine_cook` links engine_core alone. Failed cooks
must delete stale output; cooked formats carry versioned headers.

### Derived Data Cache (content-addressed cooking)
Cooking is a caching problem: cooked output is a pure function of
(source bytes ⊕ cooker id ⊕ cooker version ⊕ settings), so that hash — the
DDC key (BLAKE3-256) — names the output in a two-tier blob store
(`assetlib/ddc.h`). Local tier `~/.engine/ddc` (override `ENGINE_DDC`) is
per-machine, shared across projects; optional shared tier `ENGINE_DDC_SHARED`
(any network mount) is the studio cache — a hit there means a teammate/CI
already cooked it and nobody compresses that 8K texture twice. Staleness is
simply `record.ddcKey != currentKey`; wiping `.cache/` re-materializes by
hardlink without recooking; a per-cooker version bump re-cooks ONLY that
cooker's assets. A cook RECORD is a manifest of member blobs (cooked mesh +
its sibling `.ctex` embedded textures — cookers report extras via
`CookContext::addOutput`), fetched all-or-nothing. Cookers write to a TEMP
path, the pipeline ingests then hardlink-materializes — never hand a cooker a
hardlinked final path (an ofstream would truncate the blob for every project;
blobs are also stored chmod 0444 for exactly that reason). Cook identity per
cooker: `id()` + `version()` + `settingsFingerprint(ctx)` — the fingerprint
MUST cover every env knob that alters output (`COOK_TEX_HQ`, the normal-map
filename heuristic), or a fast-quality blob silently satisfies a final-bake
request. Failed cooks store no blob but record the key: identical inputs are
not retried until source/cooker/settings change (`forceRecook` evicts the
local blob and bypasses the fetch path — otherwise it would just re-download
the bytes under suspicion).

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
