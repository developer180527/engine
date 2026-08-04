---
status: as-built
tier: working
verified: 2026-08-04
covers:
  - src/render/
tests:
  - tests/gpu_cache_test.cpp
  - tests/render_registry_test.cpp
  - tests/render_pipeline_test.cpp
# `working` as of 2026-08-04, and the thing that changed is the one this note has
# been naming as the blocker for weeks: SUBMISSION is tested. It was untestable
# because bgfx's Noop backend never sets numDraw (its submit() writes timing,
# zeroes numPrims, and touches nothing else), so draw counts cannot be read back
# from bgfx headlessly. render/submit_stats.h counts on OUR side of the API, which
# works on any backend — and render_pipeline_test builds a RenderView by hand (no
# ECS, no Renderer, no window) and asserts the submission decisions: cull wiring,
# batch runs, the instanced path AND its negative case, the uniform-buffer draw
# ceiling, R4's one-palette-per-skinned-item invariant, the shadow pass's separate
# light-space cull, and that the counters reset per view. Mutation-checked: dropping
# render()'s stats reset, and disabling the instanced path, each fail it.
#
# NOT `hardened`, and the gap is specific: no pixels. Noop executes nothing, so
# "the shadow map is sampled with the right matrix" and "vs_instanced.sc reads
# i_data0..3" are outside any headless test's reach — instancing, the shadow cull
# and shadow instancing are verified by COUNTS and timings, never visually. That
# needs a real-device harness (golden-image or GPU-timing based), and it is the
# next thing that would raise this tier. There is also no fuzz/soak/stress lane
# over the render path.
#
# INSTANCING (R5) is live: the submit loop walks rworld::batchRunLength and
# collapses runs sharing mesh+material into one instanced submit (vs_instanced.sc,
# model matrix from instance data). 20 001 objects -> 1 draw call. Skinned items,
# submeshes and data-driven materials fall through to per-draw on purpose.
#
# MEASURED on the stress scene (scripts/gen_stress_scene.py), which is reproducible
# in a way a hand-built project is not. At 20 000 objects the Render.* zones read
# extract 1.13 ms, cull 0.52, shadow 0.28, submit 0.13 — the whole render path is
# ~2.1 ms and the frame is vsync-bound. Extraction went 15.2 -> 1.1 ms over five
# changes (issues.md R14/R15); it is now parallel over archetype chunks on
# engine::jobs, and so is the frustum cull (R16: 3.9 ms -> 0.83, with std::sort
# replaced by a stable radix sort, 1.15 -> 0.22). The cull now reads SoA streams
# (world/issues.md A2.P1) rather than the 144-byte RenderItem, which is what took the
# SHADOW pass from 0.48 ms to 0.15 at 100 k — its cull was rebuilding every bounding
# sphere the camera pass had just built. At 100 000 objects: extract 5.41, cull 1.37,
# shadow 0.15, submit 0.36, render 7.29, frame ~9.0 ms in ONE draw call. Extraction is
# the largest phase and is already parallel. NEON was measured and DECLINED (the plane
# arithmetic is ~4% of the cull, so 2.3x on it is 0.4% of the frame), and RenderItem is
# down to 128 bytes with two write-only fields deleted (A3). Both were small, and A3
# records why: extraction is parallel over 12 cores, so per-item savings divide by the
# core count. The next lever is incremental extraction, not micro-optimisation.
# R7 (material-bind dedup) is still open: materialBinds == draws on every
# non-instanced path.
---
# Render

## Purpose
Everything between the ECS and the GPU: render data extraction, the swappable
pipeline, pass scheduling, GPU resource registries, and vertex formats.
Backend is bgfx (Metal on macOS, D3D/Vulkan elsewhere).

## Architecture
Two layers:

1. **`Renderer`** (`renderer.h`) — owns the device and produces the
   `RenderView`. Four concerns, one TU each, because it was one 437-line file
   mixing bgfx init with the hot loop:
   - `renderer.cpp` — pipeline ownership: attach/detach and `makeContext()`.
     Both `setShadowResolution` and `setShaderCacheRoot` live here for one
     reason: the pipeline builds programs and targets in `onAttach`, ONCE, so a
     change after init must re-attach or it silently keeps the old programs.
   - `renderer/device.cpp` — `init`/`shutdown`/`resize`/`frame`, and the bgfx
     allocator that tags every bgfx byte to the Rendering heap. Records the
     measurements behind single-threaded bgfx.
   - `renderer/targets.cpp` — the scene/game framebuffers and the three
     `render*` entry points, which differ only in which target they pick.
   - `renderer/extract.cpp` — queries `Transform + MeshRenderer` and
     `Transform + Light` into flat `RenderItem`/`LightItem` arrays. The pipeline
     never touches the ECS. Was the hot path — 15.2 ms at 20 000 objects, now
     1.1 ms — via partitioned/optional query terms, a direct SRT compose, and
     `jobs::parallelFor` over archetype chunks. The file header carries the full
     measurement history and the reason SIMD was not the answer.
2. **Pipeline** — `IRenderPipeline` (`render_pipeline.h`) is the swap point
   (`Renderer::setPipeline`). There is exactly ONE implementation,
   `ForwardPipeline`, split one concern per TU:
   - `forward_pipeline.h` — the class declaration, and nothing else.
   - `pipeline/programs.cpp` — programs, uniforms, shadow map (onAttach/onDetach).
     The only includer of `pipeline/shader_blobs.h`, whose arrays are `static`.
   - `pipeline/opaque_pass.cpp` — `render()`: visible set → material binds →
     instanced runs → submits, plus the debug-line pass.
   - `pipeline/shadow_pass.cpp` — `renderShadow()`: light matrices, light-space
     visible set, instanced casters.

   A `passes/` directory used to sit here describing a second, pass-list
   architecture — nine headers, in no CMake target, included by nothing, never
   compiled. It was deleted (see `issues.md` R11): two renderer designs in one tree
   with only one live is the thing that stops the subsystem being readable, and the
   real structure of `render()` is now known from measurement rather than guessed at
   before any existed.

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
