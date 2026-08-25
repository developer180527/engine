// ── sim_purity_check — compile-enforced sim/GPU boundary (audit A.1–A.4) ────
// This TU compiles with tests/poison/ FIRST on the include path: any header
// below that transitively includes <bgfx/bgfx.h> or <assimp/*> hits an
// #error naming the violation. These are the headers a dedicated server /
// headless sim consumes — the audit found the boundary was convention only,
// and convention had already drifted in four places (bgfx types in
// EngineRuntime's public API, a live bgfx::getCaps() in camera_util.h,
// unconditional Assimp registration, bgfx in async_loader.h). Hand-auditing
// goes stale; this target makes the next leak a build failure.
//
// NOT yet on the list (known-impure, tracked for the engine_sim lib split):
//   runtime/runtime.h            — Renderer is a by-value member
//   runtime/services/async_loader.h — AssetStorage bundles GPU registries
//   runtime/module_loader.h      — plugin.h reaches render types
// Growing this list IS the A.* migration; never shrink it.
#include "runtime/camera_util.h"          // pure math since A.2
#include "runtime/event_sweeper.h"
#include "runtime/world_query_cache.h"
#include "runtime/jobs/jobs.h"
#include "runtime/input/input.h"
#include "runtime/input/input_manager.h"
#include "runtime/input/hid_keymap.h"
#include "runtime/input/input_sources.h"
#include "core/transform.h"
#include "core/transform_utils.h"
#include "core/memory/mem.h"
#include "core/logger.h"
#include "components/event_component.h"
#include "components/camera.h"
#include "components/spinner.h"
#include "components/character_controller.h"

#include <cstdio>

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_purity_check: OK — sim-facing headers are GPU-free "
                "(compiling IS the test)\n");
    return 0;
}
