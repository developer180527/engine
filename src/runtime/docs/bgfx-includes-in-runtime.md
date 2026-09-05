---
status: unreviewed
---
# Issues

## The Codebase Breakdown: Where the Contamination Lives

~~runtime.h (The Ultimate Bottleneck)~~ — **HEADER HALF FIXED 2026-09-04 (G1c);
the LINK half is still open.**

`runtime.h` now compiles with bgfx entirely absent from the include path,
asserted by `tests/headless_include_probe.cpp` in the `unit` lane. It still
embeds `Renderer m_renderer` by value — that turned out not to be the problem.
The problem was that `renderer.h` DECLARED bgfx handle members, and behind it
`render_view.h`, `render_context.h`, `mesh.h`, `texture.h`, `vertex.h`,
`asset_registry.h` and `primitive_library.h` each did the same. All now use
`gpu::` types.

**LINK HALF CLOSED 2026-09-05 (G1c step B).** `engine_runtime` still links bgfx,
which is correct — it is the client library. Beside it there is now
`engine_runtime_server`, built from the same source list minus the ten TUs that
name a graphics API:

```
engine_host         4030 bgfx symbols   Cocoa, Metal, QuartzCore   29 MB
server_link_probe      0 bgfx symbols   (no graphics frameworks)   13 MB
```

A separate target rather than an option, so both build on every configure and
the server cannot rot. Three null implementations carry it — `gpu_null.cpp`,
`NullRenderer` and `window_ops_null.cpp` — and two `#if`s, both at factories
(`initRenderer()` and `platform_factory`). Asserted by
`tests/server_link_probe.cpp`, which boots and ticks the engine rather than
merely linking it, since a static library only pulls the objects an executable
references.

**The whole of this document is now historical.** Every entry above is fixed.

**Why the header half was worth doing on its own:** a transitive include is
invisible to review and to grep. Every one of the eight headers above was found
by walking `-H` output by hand, one at a time; the probe now finds all of them
at once, and keeps finding them. Because the core runtime directly orchestrates the renderer's lifecycle, any machine trying to spin up the engine must have access to graphics libraries. If the dedicated server tries to instantiate EngineRuntime to run a game loop, it forces a graphics pipeline instantiation into existence.

~~camera_util.h (Math Holding Hands with Graphics)~~ — **FIXED, and left here as the
worked example of what the rest of this list looks like once done.** It used to
include <bgfx/bgfx.h> solely to read one cap, `bgfx::getCaps()->homogeneousDepth`,
so evaluating a projection matrix required an initialised GPU device. It now takes
`bool homogeneousDepth` as a PARAMETER and the render path passes the cap in; the
header records this as audit A.2. The fix cost one parameter, and that is the shape
every other entry below wants: the graphics fact is supplied by the caller that
already knows it, rather than queried by a header that should not care.

~~async_loader.h & asset_service.cpp (Mixing State and Bytes)~~ — **FIXED
2026-09-04 by G1's first part.** The asset system used to pull double-duty:
`MeshGPUData` passed `bgfx::Memory*` around and `asset_service.cpp` called
`bgfx::createTexture2D` directly, so the background loader was wired to speak
"Graphics API" instead of just processing game data.

It now goes through `render/gpu.h` — opaque handles, an opaque `gpu::Blob`
staging type, and five operations. **All five of the non-test, non-editor files
that mixed loading with upload are clean**, verified by
`scripts/check_gpu_seam.py` in the `unit` lane, which fails the build if any
file outside `src/render/` includes a graphics API again. The remaining
`bgfx::` mentions in this subsystem are prose in comments, not includes.

## The Problem Statement (the Summary Paragraph)

**Updated 2026-09-04.** Two of the three entries above are now fixed;
`runtime.h` is the last one, and it is a lifetime/ownership problem rather than
an include problem — `Renderer` is held BY VALUE, so the fix is indirection
(a pointer, or a null renderer), not a header edit.

Architectural Issue (as originally written): The direct integration of the bgfx graphics backend into core engine runtime headers—specifically runtime.h and async_loader.h (camera_util.h is fixed, above)—creates a tight compile-time and runtime coupling between the engine simulation loop and the presentation layer. This dependency contamination prevents the engine from compiling or running as a lightweight, headless dedicated server on Linux or cloud infrastructures, which typically lack display servers (X11/Wayland), graphics drivers, or windowing system contexts (GLFW). By exposing rendering-specific handles (bgfx::Memory*) and graphics capability checks within asset loading threads and math utility functions, the codebase violates the principle of separation of concerns, inducing unnecessary memory bloat, breaking automated unit-testing capabilities for headless simulation code, and imposing a severe infrastructure barrier for server-side deployments.