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
Not yet written.
