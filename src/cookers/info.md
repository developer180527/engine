# Cookers

## Purpose
Convert source assets (FBX, glTF, PNG, scene JSON) into engine-native cooked
binaries under `<project>/.cache/`, so runtime loads are a header check +
memcpy instead of a full import.

## Architecture
Each cooker implements `assetlib::ICooker` (see `modules/assetlib`):
`canCook(extension)` + `cook(source, outputPath)`.

- **`MeshCooker`** — imports via the registered importer, writes vertex/index
  buffers + submesh ranges + bounds. Returns `skipped` for skinned meshes
  (no cooked format for bone data yet — they re-import via Assimp at load).
- **`TextureCooker`** — decodes (stb) and writes GPU-ready texel data.
- **`SceneCooker`** (`scene_cooker.cpp`) — scene JSON → `.cooked` binary
  consumed by `SceneService`.

Cooking is orchestrated by `CookService` (`src/io`) through
`assetlib::CookPipeline`: staleness = source hash vs registry record;
`cookMany` parallelizes cooks across cores while keeping registry I/O on the
calling thread.

## Invariants
- Cooked output is invalidated by source-content hash, not timestamps.
- On cook failure, delete any stale cooked binary for that asset — a missing
  cooked file falls back to the importer path; a stale one renders wrong.
- Cooked formats are versioned headers; bump the version when layout changes.

## Future Work
- Skinned mesh cooking (vertex bone data + skeleton + clips in one binary).
- Shader/material cooking.
