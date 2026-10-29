---
status: plan
covers:
  - src/render/renderer_interface.h
  - docs/contracts/renderer.md
---
# The renderer contract — many renderers, one engine

> **Status: plan, 2026-10-25. Nothing here is built.** It replaces
> `IRenderer` (a null-object seam, by its own comment "not a swap point") with
> a contract any renderer can implement: the bgfx one today, the librhi AAA
> renderer, a WebGPU renderer for the web, and later voxel, terrain, 2D or
> single-platform renderers, each built its own way.

## 1. Why now

Today the boundary is the bgfx renderer's shape:

- `IRenderer::init` takes the engine's four GPU registries, whose handles are
  bgfx's (`gpu::`).
- It asks for `homogeneousDepth()` (an OpenGL depth-range idea),
  `setShadowResolution`, `sceneColorTexture()` as a `gpu::TextureHandle`, and
  `submitDraw(mesh, material)`.
- The engine extracts a mesh-only `RenderView` and hands it down, so a renderer
  cannot see voxels, terrain or anything else the engine did not anticipate.
- `runtime_boot.cpp` picks `Renderer` or `NullRenderer` in code; physics and
  audio are picked by name in `project.json`.
- The editor shows viewports through `gpu::TextureHandle`, so the editor's UI
  is tied to whichever GPU API the renderer uses.

The second real renderer (librhi) should arrive behind a contract, not copy
this shape into one.

## 2. The principle

**The engine owns the world and the frame; a renderer owns everything on the
GPU.** The engine never names a GPU API, a GPU handle, a mesh layout or a
render pass. A renderer never writes the world.

## 3. The contract, in six parts

### 3.1 Selection and capabilities
- A renderer is a **provider**, chosen in `project.json`
  (`"renderer": "forward-bgfx" | "aaa" | "web" | "voxel" | "null"`), from a
  registry, exactly as physics and audio are (`stock_plugins.h`).
- It reports **capabilities** at creation:
  - platforms it runs on;
  - editor support (viewports, picking, previews);
  - headless use (offscreen frames, for tests and servers);
  - the cook profiles it consumes (3.3).
  A project that asks for an editor viewport the renderer does not offer is an
  error at startup, by name, never a black window.

### 3.2 Input: the world, read-only, plus a change stream
- The renderer gets **read access to the ECS world** for the duration of
  extraction, at one point in the frame (3.6), and never a write.
- It also gets a **change stream per component type**: created, changed,
  destroyed, with stable ids. This is the retained scene's table and ids
  (WO-019, `docs/contracts/render-scene.md`) made general, so a renderer
  keeps its own persistent GPU scene and touches only what changed.
- **Each renderer extracts what it understands** and ignores the rest: the AAA
  renderer reads meshes and lights, a voxel renderer reads voxel volumes, a 2D
  renderer reads sprites. A component nobody renders is not an error.
- The engine still owns what is common to every renderer, and passes it as a
  **frame packet**: the views to draw (camera, viewport, target), the display
  transform (exposure, tone mapper, grade, HDR output), time and
  interpolation alpha, and debug-draw requests.

### 3.3 Assets: CPU in the engine, GPU in the renderer
- The engine owns **cooked CPU assets** and their lifetime (the asset service,
  `docs/contracts/asset-service.md`): meshes, textures, materials as data.
- A renderer owns **GPU residency**: it uploads, streams and evicts as it
  likes, keyed by the engine's asset ids.
- A renderer **declares the cook profiles it needs** (for example "BC7 + mesh
  clusters" for the AAA renderer, "ASTC + plain meshes" for the web one,
  "voxelised" for a voxel one). The cooker produces them per renderer and per
  platform. This is also what stops assets one renderer needs from leaking into
  a build that uses another.
- The four GPU registries (`AssetRegistry`, `TextureRegistry`,
  `MaterialRegistry`, `SkeletonRegistry`) move inside the bgfx renderer: they
  are its residency, not the engine's.

### 3.4 Output: surfaces the engine gives
- The engine provides **surfaces**: the game window, each editor viewport, and
  offscreen captures (tests, thumbnails).
- The renderer draws views into them. It never creates windows.
- The editor shows renderer output through **an image-sharing interface the
  renderer implements** (a UI texture the editor's UI layer can draw, from the
  renderer's own device), never through a GPU handle type in an engine header.

### 3.5 Optional parts
Each a separate interface a renderer may implement, found by query, so the
editor and tools adapt to what is there:
- picking (an entity id under a pixel);
- debug drawing (lines, shapes; `core/debug_draw.h`);
- statistics (draws, GPU time, memory) for the profiler channel;
- hot reload of shaders and materials;
- asset previews and thumbnails.

### 3.6 Frame, threads, errors
- **Frame:** extract (world read, on the engine's frame thread, between
  simulation and the next tick), then submit (renderer's own threads, no world
  access), then present. Extraction has a budget the profiler reports.
- **Threads:** the renderer may use the engine's job pool (`engine::jobs`) or
  its own; it must not block the frame thread past extraction.
- **Errors:** a lost device or failed init is reported through the contract
  (an error result and a reason), and the engine decides: fall back to the null
  renderer and say so, or stop.
- **Pacing:** the engine owns frame pacing (WO-061); a renderer reports
  whether it presented, so the limiter knows.

## 4. What stays as decided: no binary renderer ABI yet

DR-0006 refused a frozen renderer **binary** ABI. This plan keeps that:
renderers implement a **C++ source contract**, compiled with the engine (as
providers are today). The difference from today is the shape: the contract
names no GPU API and no mesh model, so it can hold many renderers. A binary
ABI (renderers as `.so`/`.dll` built elsewhere) stays a later, separate
decision, reversed only when the contract has held across three renderers.

## 5. How we get there

1. **This plan agreed**, then a decision record that supersedes the shape of
   `IRenderer` (not DR-0006).
2. **The contract written as headers**, with a conformance test suite every
   renderer passes; `NullRenderer` is the first to pass it.
3. **The bgfx renderer moved behind it** as renderer #1. Proof: the same
   pixels (the existing golden images and the 50 000-object scene unchanged).
4. **The editor's viewports moved to the image-sharing interface.**
5. **The librhi AAA renderer** as #2, in its own project.
6. **The WebGPU renderer** (`webgpu.h`, the web) as #3.

Each step leaves a working engine; none ships half-built.

## 6. Questions for the owner

1. The renderer name in `project.json` per project, or per platform (a project
   that uses the AAA renderer on desktop and the web renderer in the browser)?
   Recommended: **per platform, with a default**.
2. Should a project be able to use two renderers at once (the game in one,
   the editor's tool views in another)? Recommended: **not in v1**.
3. Is the editor itself required to run on every renderer, or only on the
   desktop ones? Recommended: **desktop ones only**; the web renderer is for
   shipped games.
