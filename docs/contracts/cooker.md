---
status: as-built
contract: cooker
kind: interface
state: provisional
owner: modules/assetlib
header: modules/assetlib/include/assetlib/cooker.h
implementations:
  - real: src/assets/cookers/texture/texture_cooker.h
  - real: src/assets/cookers/mesh/mesh_cooker.h
  - real: src/assets/cookers/material/material_cooker.h
  - real: src/assets/cookers/shader/shader_cooker.h
tests:
  - tests/cooker_test.cpp
covers:
  - modules/assetlib/include/assetlib/cooker.h
verified: 2026-09-27
---

# cooker — source assets to runtime formats

`assetlib::ICooker`: one per asset type, run by the cook pipeline, keyed by a
fingerprint so a change in the cooker's inputs or version re-cooks.

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
A cook returns `CookResult{success=false, error}` with a human-readable reason;
`skipped` means "not this cooker's type", and `cancelled` means host shutdown,
never a verdict on the asset.

**A feature in the source that the cooked format cannot carry is a refusal that
names what was lost, never a success.** A cooked asset that silently lacks part
of its source is worse than no asset: nothing downstream can tell. Current case:
a skinned or animation-only glTF (`src/assets/importers/gltf_losses.h`, WO-002,
pinned by `tests/cooker_test.cpp` §2c). The one sanctioned exception is a loss
that leaves the asset *correct but less*: node animations on a static glTF mesh
cook as the static mesh and are reported in the cook log.
