---
status: as-built
tier: prototype
verified: 2026-08-01
covers:
  - src/render/
tests:
  - tests/gpu_cache_test.cpp
# STILL `prototype`, deliberately, even though a test now exists. gpu_cache_test
# covers GpuResourceCache — a new component that is not yet wired into any
# render path. The PIPELINE (extraction, culling, sorting, submission, material
# binding) remains untested, and that is what this subsystem mostly is.
# Promoting on the strength of a test for an unwired component would be gaming
# the ladder. See docs/renderer-audit-and-plan.md: 9 ranked findings, 3
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
