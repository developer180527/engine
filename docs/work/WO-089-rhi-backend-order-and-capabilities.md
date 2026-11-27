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
  - src/core/device_select.h
  - src/project/project_context.h
  - src/project/provider_resolve.cpp
  - src/render/renderer/device.cpp
  - src/runtime/runtime_boot.cpp
  - src/runtime/renderer-forward-bgfx.provider.json
  - cmake/EngineProviders.cmake
  - tests/device_select_test.cpp
source: provider-selection.md §2 and §4 (DR-0037); research 2026-11-26 (Godot's fallback_to_* settings; bgfx and SDL3 GPU try the next backend; Unity's ordered graphics API list)
---
## Why
The graphics API is not a provider. It is a backend inside librhi, so the D3D12 code never meets the Mac build. Two choices remain at startup, because only the device knows them: which compiled backend creates a device, and whether the GPU has what the chosen renderer needs. Today neither is declared. The engines studied either fall back silently along a fixed chain (Godot's `fallback_to_*`, bgfx) or let the project order the list (Unity). We take Unity's model: the project's ordered list is the only fallback, and a missing capability is refused by name or met by the renderer's declared fallback.

## Done when
- [x] `project.json` takes an optional `"rhi"` object: an ordered list of backends per platform. Leaving it out takes librhi's per-platform default. A backend not compiled for that platform is refused at `engine_build`, through the resolver (WO-086)
- [x] at startup the engine tries the listed backends in order and logs which one was used and why each earlier one was not; past the end of the list, it refuses by name with what was tried
- [x] a renderer's manifest names the device capabilities it needs. At startup a missing one is either refused by name or met by the renderer's declared fallback (`engine-architecture.md` §6), never silently
- [x] a test with a fake device that lacks one capability covers both outcomes

## Contract
Without `"rhi"` in `project.json`, the backend choice is what it is today. On Apple platforms there is one backend, Metal, and the list is not read.

## Not in scope
- Which backends librhi has: `docs/rhi/`. This order only chooses among the ones it was built with.
- Today's bgfx renderer picks its own backend; this applies to it only as far as bgfx allows, recorded in the order when started.

## Notes (2026-11-27; left `todo`, not `active`: two orders are active already. Close once CI is green on every leg)
- **Where it lives:** `src/core/device_select.h` (`rhisel`), pure. The resolver calls it at `engine_build`, and the runtime calls it again at startup, in every player. A shipped player contains no resolver (DR-0032, the WO-088 symbol check) but still reads its `project.json`, so the check cannot live in the resolver alone. This adds the module edge `src/project -> src/core`, recorded in the audit baseline.
- **The default is ONE backend.** A platform table's `gpu_apis` lists what the platform has (Windows: d3d12, d3d11, vulkan). Taking the whole list as the default would have given every existing project a fallback chain it never asked for, against this order's contract. With no `rhi` block, the order is the table's first backend alone: d3d12, vulkan, metal, as the old `#if` chose. A fallback happens only when the project lists one. The plan's §4 says so now.
- **bgfx falls back on its own.** Asked for one backend, `bgfx::init` tries every other it was built with when that one fails, ending at Noop, and says so only in a trace (`rendererCreate`, scored). Each attempt now checks `bgfx::getRendererType()`. If bgfx made a different backend, it is shut down and the attempt counts as failed, "bgfx fell back to X on its own". After a failed init bgfx keeps its single-threaded flag, and `shutdown` clears it, so `renderFrame()` is called again only after a shutdown, or bgfx's render-thread check would trip.
- **Capabilities:** the engine's names are in `rhisel::capabilities()`. forward-bgfx needs `index32` and `depth_d32f` (the shadow map's D32F target), and has fallbacks for `instancing` (per-draw submits), `render_rgba16f` (an RGBA8 scene target) and `texture3d_rgba16f` (no grading LUTs). Those three fallbacks already existed in the passes; they were simply undeclared. An unknown name, or one both needed and given a fallback, is refused when the manifest is read. `IRenderer::initError()` carries the refusal to the runtime's boot error.
- **Apple:** the contract said the list "is not read" there. It is read and checked: `"macos": ["vulkan"]` is refused by name ("macos has metal"), because ignoring a wrong value would be the silent fallback this plan forbids.
- **Checked:** `device_select_test` (28 checks: the block's shape, the order, a fake device where backends fail and nothing past the list is tried, a fake device lacking `instancing` taken both ways, the manifest reader, and the resolver as `engine_build` runs it). On this Mac: `engine_host` logs `graphics backend: metal (the macos default: metal)`. A project with `"rhi": {"macos": ["vulkan"]}` is refused by name both by `engine_host` and by `engine_build --explain-providers`. `--platform windows` with `["vulkan", "d3d12"]` explains `rhi  vulkan, d3d12 (project.json "rhi".windows)`. `providers.lock.json` records the order.
- **Not exercised on a real GPU:** a backend failing, and a device lacking a capability. CI has no GPU, and this Mac has every capability, so both are covered only by the fake device.
