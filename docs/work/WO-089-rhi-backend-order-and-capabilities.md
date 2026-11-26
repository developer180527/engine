---
status: plan
id: WO-089
title: librhi's backend order from project.json, and device capabilities refused by name
program: renderer
priority: P2
size: M
state: todo
depends: [WO-086]
touches:
  - src/project/project_context.h
  - src/runtime/runtime_boot.cpp
source: provider-selection.md §2 and §4 (DR-0037); research 2026-11-26 (Godot's fallback_to_* settings; bgfx and SDL3 GPU try the next backend; Unity's ordered graphics API list)
---
## Why
The graphics API is not a provider. It is a backend inside librhi, so the D3D12 code never meets the Mac build. Two choices remain at startup, because only the device knows them: which compiled backend creates a device, and whether the GPU has what the chosen renderer needs. Today neither is declared. The engines studied either fall back silently along a fixed chain (Godot's `fallback_to_*`, bgfx) or let the project order the list (Unity). We take Unity's model: the project's ordered list is the only fallback, and a missing capability is refused by name or met by the renderer's declared fallback.

## Done when
- [ ] `project.json` takes an optional `"rhi"` object: an ordered list of backends per platform. Leaving it out takes librhi's per-platform default. A backend not compiled for that platform is refused at `engine_build`, through the resolver (WO-086)
- [ ] at startup the engine tries the listed backends in order and logs which one was used and why each earlier one was not; past the end of the list, it refuses by name with what was tried
- [ ] a renderer's manifest names the device capabilities it needs. At startup a missing one is either refused by name or met by the renderer's declared fallback (`engine-architecture.md` §6), never silently
- [ ] a test with a fake device that lacks one capability covers both outcomes

## Contract
Without `"rhi"` in `project.json`, the backend choice is what it is today. On Apple platforms there is one backend, Metal, and the list is not read.

## Not in scope
- Which backends librhi has: `docs/rhi/`. This order only chooses among the ones it was built with.
- Today's bgfx renderer picks its own backend; this applies to it only as far as bgfx allows, recorded in the order when started.
