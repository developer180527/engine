---
status: unreviewed
---
# Issues

## The Codebase Breakdown: Where the Contamination Lives

runtime.h (The Ultimate Bottleneck): the entire EngineRuntime class directly embeds Renderer m_renderer; and hard-includes <bgfx/bgfx.h>. Because the core runtime directly orchestrates the renderer's lifecycle, any machine trying to spin up the engine must have access to graphics libraries. If the dedicated server tries to instantiate EngineRuntime to run a game loop, it forces a graphics pipeline instantiation into existence.

~~camera_util.h (Math Holding Hands with Graphics)~~ — **FIXED, and left here as the
worked example of what the rest of this list looks like once done.** It used to
include <bgfx/bgfx.h> solely to read one cap, `bgfx::getCaps()->homogeneousDepth`,
so evaluating a projection matrix required an initialised GPU device. It now takes
`bool homogeneousDepth` as a PARAMETER and the render path passes the cap in; the
header records this as audit A.2. The fix cost one parameter, and that is the shape
every other entry below wants: the graphics fact is supplied by the caller that
already knows it, rather than queried by a header that should not care.

async_loader.h & asset_service.cpp (Mixing State and Bytes): the asset system is pulling double-duty. Instead of just loading raw bytes from the .cooked files into plain old memory, structures like MeshGPUData are passing around bgfx::Memory* objects, and asset_service.cpp is directly calling bgfx::createTexture2D. This means the background thread loader is fundamentally wired to speak "Graphics API" instead of just processing raw game data.

## The Problem Statement (the Summary Paragraph)

Architectural Issue: The direct integration of the bgfx graphics backend into core engine runtime headers—specifically runtime.h and async_loader.h (camera_util.h is fixed, above)—creates a tight compile-time and runtime coupling between the engine simulation loop and the presentation layer. This dependency contamination prevents the engine from compiling or running as a lightweight, headless dedicated server on Linux or cloud infrastructures, which typically lack display servers (X11/Wayland), graphics drivers, or windowing system contexts (GLFW). By exposing rendering-specific handles (bgfx::Memory*) and graphics capability checks within asset loading threads and math utility functions, the codebase violates the principle of separation of concerns, inducing unnecessary memory bloat, breaking automated unit-testing capabilities for headless simulation code, and imposing a severe infrastructure barrier for server-side deployments.