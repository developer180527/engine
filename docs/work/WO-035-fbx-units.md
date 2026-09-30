---
status: plan
id: WO-035
title: FBX units — decide whether imports arrive in metres
program: assets
priority: P2
size: M
state: todo
touches:
  - src/assets/import/frontend_assimp.cpp
source: found 2026-09-30 while building the Assimp front end (WO-013); imported-scene.md §8 Q1
---
## Why
Assimp's FBX reader applies the file's `UnitScaleFactor` in centimetres, so an FBX arrives in the units it was authored in. That's usually centimetres, 100x the engine's metres, unless the exporter baked a conversion. The engine has always cooked FBX that way, and projects compensate with entity scale.

Converting to metres is right for the engine. It is also a change to every FBX in every project, so it is a decision, not a refactor. WO-013 kept the old behaviour on purpose.

## Done when
- [ ] measured: the `UnitScaleFactor` and resulting size of every FBX in the tree and in fps_shooter, recorded
- [ ] decided, as a decision record: convert to metres at import (Assimp's `AI_CONFIG_FBX_CONVERT_TO_M`, or the front end scaling the root), or keep authored units and say so in the docs
- [ ] if converting: the cooker version bumps, and a migration note lists what projects must change (entity scales that compensated)
- [ ] the contract suite's `AuthoredCentimetreZUp` case gains an FBX variant if an FBX can be produced for it; COLLADA already passes it
