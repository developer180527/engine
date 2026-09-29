---
status: plan
id: WO-015
title: Animation takes engine types, not Assimp types
program: assets
priority: P2
size: M
state: todo
depends: [WO-013]
touches:
  - src/animation/ozz_bridge.h
  - src/animation/assimp_skeleton_loader.h
  - src/animation/clip_library.h
source: review 2026-09-29 C3
---
## Why
`buildOzzClip(aiAnimation*)` and `extractSkeleton(aiScene*)` tie the animation module to one parser.

## Done when
- [ ] `buildOzzClip` and `extractSkeleton` take `ImportedScene` clip and skeleton types
- [ ] `assimp_skeleton_loader.h` is gone, or it lives inside the Assimp front end
- [ ] no Assimp include in `src/animation/` (audit rule)
- [ ] existing animation tests are unchanged and green
