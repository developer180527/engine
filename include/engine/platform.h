#pragma once
// Platform layer: how the engine gets a window (or doesn't).
//
//   GlfwPlatform      — default; stock OS window (what EngineRuntime::init
//                       creates when you don't pass a platform)
//   HeadlessPlatform  — no window, no GPU; dedicated servers / CLI tools
//   IPlatform         — implement yourself to embed the engine in an
//                       existing native window

#include "runtime/platform/platform.h"
#include "runtime/platform/glfw_platform.h"
#include "runtime/platform/headless_platform.h"
