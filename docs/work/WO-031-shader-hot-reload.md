---
status: plan
id: WO-031
title: Shader hot-reload — the runtime picks up a re-cooked shader
program: renderer
priority: P3
size: M
state: todo
contracts: [render-pipeline]
touches:
  - src/render/pipeline/programs.cpp
  - src/render/shader/info.md
source: docs/process/roadmap.md, old §4 "Standing gaps"; src/render/shader/info.md "No hot reload"
---
## Why
The cook pipeline already notices an edited shader and re-cooks it. The running editor ignores the result, so every shader edit costs a restart.

## Done when
- [ ] a re-cooked `.cshader` replaces the live program at the next frame boundary, with no restart
- [ ] a shader that fails to cook leaves the previous program running and logs why
- [ ] `src/render/shader/info.md` no longer lists "No hot reload"

## Contract
Nothing: without a cook watcher (a shipped player, a server), the program set is fixed at startup and nothing polls. A reload never swaps a live program for nothing: a failed cook keeps the old one.
