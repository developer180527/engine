# Loaders

## Purpose
Load cooked binaries from `<project>/.cache/` into GPU-ready engine objects —
the fast path that skips importers entirely.

## Contents
- **`mesh_loader.cpp/.h`** — reads a `.cooked` mesh (header + vertex/index
  blobs + submesh ranges + bounds), creates bgfx buffers, returns a `Mesh`.

## Data Flow
`AssetService`/`AsyncLoader` checks the registry: if a fresh cooked binary
exists, the loader handles it (memcpy-fast); otherwise the matching importer
(`src/io`) re-imports the source. Skinned meshes currently always fall back
to the importer (cooker skips them).

## Rules
- Validate the cooked header version before trusting the layout; a mismatch
  means "treat as missing" and fall back to import.
- GPU buffer creation stays on the main thread (loader returns CPU blobs to
  the upload queue when called from a worker).
