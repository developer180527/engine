---
status: plan
id: WO-075
title: The renderer contract as headers, with a conformance suite the null renderer passes first
program: renderer
priority: P1
rank: 6
size: M
state: todo
depends: [WO-073]
contracts: [renderer]
touches:
  - src/render/renderer_interface.h
  - src/render/renderer_null.h
  - src/runtime/runtime_boot.cpp
source: docs/plans/renderer-contract.md §5 steps 1-2; roadmap §2.8
---
## Why
`IRenderer` is shaped like bgfx (it takes the engine's bgfx-handle registries, asks for `homogeneousDepth`, returns `gpu::TextureHandle`s), and `runtime_boot.cpp` picks a renderer in code. The librhi renderer and a web renderer both need a contract that names no GPU API; it should be written and proven before either is built on it.

## Done when
- [ ] a decision record superseding `IRenderer`'s shape (not DR-0006), after the owner answers `renderer-contract.md` §6
- [ ] the contract as headers: selection and capabilities, input as the read-only world plus the change stream (WO-072), CPU assets in the engine and GPU residency in the renderer, surfaces from the engine, optional parts by query, frame and error rules
- [ ] a conformance suite every renderer must pass, and the null renderer passes it
- [ ] the renderer chosen by name in `project.json` through WO-074's registry, refused by name when unknown
- [ ] the editor-viewport question (how a renderer's output reaches the editor's UI across APIs) settled by a short study before the bgfx renderer moves

## Contract
Nothing: the bgfx renderer keeps running behind `IRenderer` until it moves behind the contract (`renderer-contract.md` §5 step 3, a later order).
