// ── platform_factory — resolves ENGINE_WINDOW_BACKEND to a concrete platform ──
// The ONE place that names a windowing backend class. Apps call
// makeDefaultPlatform(); flipping the CMake option moves the whole engine.
#include "runtime/platform/platform.h"

// A server build has no windowing library at all — not GLFW, not SDL, and on
// Linux that is the difference between a binary that starts on a headless box
// and one that fails to load libX11. So the third choice here is "no window
// system", and like the renderer's factory in runtime_boot.cpp this is where
// the build-time decision belongs: one #if, at the point of the choice.
#if ENGINE_SERVER_BUILD
    #include "runtime/platform/headless_platform.h"
#elif defined(ENGINE_WINDOW_BACKEND_SDL3)
    #include "runtime/platform/sdl3_platform.h"
#else
    #include "runtime/platform/glfw_platform.h"
#endif

std::unique_ptr<IPlatform> makeDefaultPlatform() {
#if ENGINE_SERVER_BUILD
    return std::make_unique<HeadlessPlatform>();
#elif defined(ENGINE_WINDOW_BACKEND_SDL3)
    return std::make_unique<Sdl3Platform>();
#else
    return std::make_unique<GlfwPlatform>();
#endif
}

const char* windowBackendName() {
#if ENGINE_SERVER_BUILD
    return "headless";
#elif defined(ENGINE_WINDOW_BACKEND_SDL3)
    return "sdl3";
#else
    return "glfw";
#endif
}
