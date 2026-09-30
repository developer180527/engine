---
status: as-built
contract: mesh-importer
kind: interface
state: provisional
owner: src/assets
header: src/assets/importers/mesh_importer.h
implementations:
  - real: src/assets/importers/gltf_importer.h#GltfImporter
  - real: src/assets/importers/assimp_importer.h#AssimpImporter
tests:
  - tests/import_test.cpp
covers:
  - src/assets/importers/mesh_importer.h
verified: 2026-09-27
---

# mesh-importer — source formats in

`MeshImporter`, registered in `ImporterRegistry`: which file extensions an
importer supports, and loading a source file into engine storage. glTF and
Assimp today; a headless runtime registers none (a server loads cooked data
only), and shipping builds compile them out.

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
`MeshImportResult::fail(reason)` for a file that cannot be loaded at all.

A file that loads *partially* — features in it the importer does not read —
still loads, and the importer says what it left out, once per file, as a
warning. Current case: `GltfImporter` reads meshes only, so skins and
animations are reported and the static geometry loads
(`src/assets/importers/gltf_losses.h`, WO-002). The cooker refuses the same
files outright; the importer does not, because a scene should still open.
