---
status: plan
id: WO-047
title: Scene serialization stops reaching into runtime services; the module graph is gated acyclic
program: process
priority: P2
size: M
state: todo
depends: [WO-046]
touches:
  - src/scene/entity_serializer.h
  - src/scene/scene_serializer.h
  - scripts/engine_audit.py
source: architecture audit 2026-09-30
---
## Why
The last back-edge in the module cycle (WO-046 has the other four) is `scene <-> runtime`.

`runtime` uses `scene` to load and snapshot worlds, which is the right direction. But the serializers reach back UP into runtime services: `entity_serializer.h` includes `runtime/services/asset_service.h`, and `scene_serializer.h` includes `async_loader.h` and `asset_service.h`. Loading a scene resolves its assets by calling the services directly.

Once WO-046 and this are done the graph is a hierarchy. Then it should STAY one, which today nothing checks.

## Done when
- [ ] `src/scene` includes nothing from `src/runtime`: the serializers take what they need to resolve assets as a narrow interface or callbacks the runtime supplies (the host resolves, the serializer describes), and the `scene -> runtime` edge leaves the baseline
- [ ] a new audit rule, LAYER-06, fails if the module graph has a cycle, naming the cycle and the files that create each edge in it
- [ ] `docs/process/architecture-audit.md` lists LAYER-06 and states the layer order the graph now has

## Contract
Changes how scene loading reaches assets: through what the runtime hands the serializer, not by including the runtime.

Nothing: a scene loads the same entities with the same assets; the runtime supplies the resolver it already owns.
