---
status: plan
id: WO-017
title: Cook library split — the runtime never links the cook stack
program: assets
priority: P2
size: L
state: todo
depends: [WO-011]
touches:
  - src/CMakeLists.txt
source: review 2026-09-29 R3 (roadmap "P2 cooker split")
---
## Why
The cookers live in `engine_core`, which links Assimp and the texture encoders `PUBLIC`. So every runtime in the default build, `engine_player` included, carries the whole cook stack. Today the split is a build setting; it should be a library boundary.

## Done when
- [ ] an `engine_cook` library holds the cookers, front ends and encoders; `engine_core` links none of them
- [ ] the editor links both; `engine_player` and the server link `engine_core` only
- [ ] the existing CI symbol check (no Assimp symbols in shipping binaries) also runs on the default-build `engine_player`
- [ ] LAYER-03 declares the new edges; the audit baseline doesn't grow
