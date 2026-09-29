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
Not yet written.
