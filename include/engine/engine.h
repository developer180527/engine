#pragma once
// ── Engine SDK ───────────────────────────────────────────────────────────────
// Single include to build a game:
//
//   #include <engine/engine.h>
//
//   int main() {
//       EngineRuntime engine;
//       if (!engine.init({})) return 1;        // window + project + assets
//       engine.startSimulation();              // boot = play (in-place)
//       engine.run([&](float dt) {
//           engine.tick(dt);                   // systems + sim + primary-
//       });                                    //   camera render to screen
//       engine.shutdown();
//   }
//
// Granular headers if you don't want everything:
//   <engine/components.h>  ECS components (Transform, Camera, Light, ...)
//   <engine/plugin.h>      IEnginePlugin + PluginRegistry
//   <engine/platform.h>    IPlatform implementations (GLFW, headless)
//   <engine/input.h>       action/axis input bindings
//   <engine/services.h>    AssetService + SceneService
//   <engine/render.h>      IRenderPipeline extension point
//
// This file (and everything it includes) is the supported API surface.
// Headers under src/ not re-exported here are internal and may change
// without notice.

#include "runtime/runtime.h"
#include "runtime/camera_util.h"

#include "engine/platform.h"
#include "engine/plugin.h"
#include "engine/components.h"
