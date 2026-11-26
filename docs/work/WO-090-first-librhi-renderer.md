---
status: plan
id: WO-090
title: The first librhi renderer — a provider beside bgfx, the forward pipeline ported pass by pass, each bgfx assumption removed where the port meets it
program: renderer
priority: P1
size: L
state: todo
depends: [WO-071, WO-075]
contracts: [renderer, gpu-seam]
touches:
  - CMakeLists.txt
  - src/runtime/
  - src/assets/cookers/shader/
  - src/assets/cookers/material/
  - src/render/
  - src/editor/imgui/
source: conversation 2026-11-26 ("fix the engine first, or build a AAA renderer first?"); docs/rhi/migration.md (strangle, never rewrite); docs/rhi/phases.md G3 and G4; WO-027's split; librhi at eafb2e6 (ABI 0.51)
---
## Why
librhi (github.com/developer180527/librhi) is the engine's bindless GPU API over Metal 4, Vulkan 1.3 and D3D12, and it has never had a real consumer; its own freeze gate lists one. The engine has one renderer, and everything around it is shaped by bgfx: shaders are bgfx `.sc` cooked by `shaderc`, materials name texture slots, meshes carry bgfx's vertex layout, the editor's ImGui draws through bgfx, and the pipeline stops at 4 096 draws because per-draw uniforms fill Metal's fixed 8 MB scratch (`ForwardPipeline::kMaxDrawsPerFrame`).

Generalising the engine first, with no second renderer, is guessing at abstractions; a full render graph or GPU-driven renderer first would inherit those assumptions or work around them, and runs for months before it is compared with anything. So the order is the one `migration.md` already sets: a librhi renderer that does exactly what today's does, beside bgfx, chosen per project, and each bgfx assumption removed at the moment the port reaches it, with bgfx still working after every commit. The render graph, GPU-driven culling and ray tracing (G5 to G8) come after, onto a seam that has been proven by two renderers.

Not blocked by librhi's ABI being unfrozen: the engine builds librhi from source in the same build, so header and library cannot disagree. librhi is pinned to one commit and moved on purpose.

## Done when
- [ ] librhi pinned as a dependency at one commit, built from source with the engine on macOS, `RHI_BUILD_TESTS` off; the pinned commit and its ABI version recorded in this order's log; the Null backend used for headless tests
- [ ] a `renderer-forward-rhi` provider (manifest, macOS first) chosen in `project.json` beside `forward-bgfx`, that opens the engine's window through `librhi_present`, clears it and draws one triangle; refused by name on a platform it was not built for (WO-086)
- [ ] shaders: one Slang source per current shader, cooked per renderer (bgfx `.sc` for bgfx, SPIR-V and metallib for librhi) through the shader cooker and DDC; the cooked shader names its renderer, so a mismatched one is refused at load by name (G3)
- [ ] materials name their textures; which slot or bindless index each lands in is the renderer's choice. The bgfx renderer still reads today's `.cmat` files unchanged
- [ ] meshes: the cook defines the vertex format, bgfx reads it as a vertex layout and librhi pulls it by index in the shader; both draw the same mesh from the same cooked file
- [ ] the forward pipeline ported pass by pass (shadow, opaque, output): per-draw data in a buffer indexed by the draw ID, no per-draw uniforms (G4)
- [ ] the two renderers compared on the same scene: the images match within a stated tolerance (a readback test), and the submit counters and GPU time are recorded side by side
- [ ] the 4 096-draw ceiling gone from the librhi renderer, shown by a scene of 20 000 unbatched draws, measured with `render_submit_bench`'s method
- [ ] the editor's ImGui drawn through whichever renderer the project uses (or the editor-viewport study's answer from WO-075, if it says otherwise)

## Contract
Nothing: a project that does not choose it gets exactly what it gets today, because `forward-bgfx` stays the default and renders as before after every commit. Each bgfx assumption is replaced in the same commit that makes both renderers use the general form.

## Not in scope
- Removing bgfx: only after the librhi renderer wins on real hardware, on a real scene, with tests green (`migration.md` step 5), a later order.
- Windows and Linux for this provider: the backends exist in librhi, but the first port is macOS, where it is tested. They follow on the same code once the farm or a rented box can run them (G0b).
- The render graph, GPU-driven culling, ray tracing, mesh shaders (G5 to G8): each its own order, after this one.
- Parallel recording: measured at 0.41 ms for 4 000 unbatched draws (`render_submit_bench`, 2026-11-26); librhi records lists on many threads when a renderer needs it, and this one does not yet.
