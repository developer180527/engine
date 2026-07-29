// ── platform_factory — resolves ENGINE_WINDOW_BACKEND to a concrete platform ──
// The ONE place that names a windowing backend class. Apps call
// makeDefaultPlatform(); flipping the CMake option moves the whole engine.
#include "runtime/platform/platform.h"

#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    #include "runtime/platform/sdl3_platform.h"
#else
    #include "runtime/platform/glfw_platform.h"
#endif

std::unique_ptr<IPlatform> makeDefaultPlatform() {
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    return std::make_unique<Sdl3Platform>();
#else
    return std::make_unique<GlfwPlatform>();
#endif
}

const char* windowBackendName() {
#if defined(ENGINE_WINDOW_BACKEND_SDL3)
    return "sdl3";
#else
    return "glfw";
#endif
}
