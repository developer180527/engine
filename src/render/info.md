---
status: as-built
tier: prototype
verified: 2026-08-04
covers:
  - src/render/
tests:
  - tests/gpu_cache_test.cpp
  - tests/render_registry_test.cpp
# STILL `prototype`, deliberately, and the reason is narrower than it was.
# Covered now: GpuResourceCache (gpu_cache_test — and it IS wired into a render
# path, the cooked-texture route through AssetService that the shipped game uses),
# and the three registries every drawn thing lives in (render_registry_test).
# What remains untested is SUBMISSION — the pipeline's binding of programs,
# uniforms and textures — and that is still the bulk of what this subsystem does.
# It is not testable headlessly: bgfx's Noop backend never sets numDraw (its
# submit() sets timing and zeroes numPrims, nothing else), so draw counts cannot
# be asserted without a real GPU harness or a counting seam in the pipeline.
# The counting seam now EXISTS (render/submit_stats.h, filled by ForwardPipeline,
# exposed via IRenderPipeline::submitStats). Counting on our side works on any
# backend including Noop, so a headless pipeline test is now possible — it needs
# the program/shader setup ForwardPipeline::onAttach wants, which is the remaining
# work. Prototype until that test exists.
#
# INSTANCING (R5) is live: the submit loop walks rworld::batchRunLength and
# collapses runs sharing mesh+material into one instanced submit (vs_instanced.sc,
# model matrix from instance data). 20 001 objects -> 1 draw call. Skinned items,
# submeshes and data-driven materials fall through to per-draw on purpose.
#
# What the seam already measures on fps_shooter (2026-08-04): 12 draws from 10
# items (1 culled), 3 batch runs — so instancing would remove 9 of 12 submits
# (R5); 12 material binds for 12 draws, i.e. no dedup at all (R7); and 1 bone
# palette upload for 1 skinned item, which is R4's invariant machine-checked.
# Promoting on the strength of a test for an unwired component would be gaming
# the ladder. See docs/plans/renderer-audit-and-plan.md: 9 ranked findings, 3
# critical. Phase 2 (VRAM census, duplicate report, leak detector over a real
# scene) is what will make a higher tier provable.
---
# Render

## Purpose
Everything between the ECS and the GPU: render data extraction, the swappable
pipeline, pass scheduling, GPU resource registries, and vertex formats.
Backend is bgfx (Metal on macOS, D3D/Vulkan elsewhere).

## Architecture
Two layers:

1. **Extraction** (driven by `Renderer` in `src/runtime/renderer.cpp`) —
   queries `Transform + MeshRenderer` and `Transform + Light`, builds a
   `RenderView` (flat `RenderItem`/`LightItem` vectors + camera matrices +
   target). The pipeline never touches the ECS.
2. **Pipeline** — `IRenderPipeline` (`render_pipeline.h`) is the swap point
   (`Renderer::setPipeline`). Default is the pass-list pipeline:
   - **`passes/i_render_pass.h`** — pass interface; passes receive a
     `PassContext` (view ids, target, view data, registries).
   - **`passes/pass_list_pipeline.h`** — runs an ordered pass list:
     shadow → sky → opaque → transparency → resolve → post.
   - `forward_pipeline.h` — the original monolithic forward renderer
     (shadow + lit opaque + skinning), still the reference implementation.

## Key Data
- **Registries** — `AssetRegistry` (meshes), `TextureRegistry`,
  `MaterialRegistry`: `Handle<Tag>` dense vectors, slot 0 = null handle.
- **Vertex formats** — `vertex.h` (static, 52 bytes), `skinned_vertex.h`
  (68 bytes: pos + normal + tangent + uv + 4×uint8 joints + 4×float weights).
- **`Mesh`** (`mesh.h`) — VB/IB handles + `SubRange` submeshes (skinned
  imports merge all body parts into one VB/IB with submesh ranges).

## Skinning (GPU)
Bone palettes are uploaded as a vec4 array uniform (`u_boneMatrices`,
128 bones × 4 vec4). **The uniform must be set before every `submit()`** —
bgfx consumes uniform state per submit, so it is set inside the submesh loop
in both the main and shadow passes. The vertex shader (`vs_skinned.sc`)
reconstructs mat4s and does 4-influence linear blend skinning.

## View IDs
0 = shadow, 1 = scene (offscreen FB), 2 = backbuffer clear, 3 = MSAA resolve,
4 = game view; 5+ allocated via `m_viewCursor`. ImGui uses high ids (editor).

## Invariants
- Row-major matrices, row-vector convention (`v * M`), matching bx.
- Scene renders into an offscreen FB at ≥ window resolution (panel downscale
  = free SSAA); recreate only on >8px delta (resize-thrash hysteresis).
- All GPU resource creation/destruction on the main thread.

## Future Work
- Pass injection API (add a custom pass without replacing the pipeline).
- Material system beyond base-color/normal (PBR params, shader variants).
