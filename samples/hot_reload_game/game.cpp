// ── Hot-reloadable game module ───────────────────────────────────────────────
// Run it:
//   build/engine_host <project-dir> build/libhot_reload_game.so
//
// Then EDIT THE CONSTANTS BELOW, rebuild just this target
// (cmake --build build --target hot_reload_game) and watch the running game
// pick the change up in about a second — same world, same entities.
//
// Reload-safety rules (see include/engine/game_module.h):
//   - state lives in components (host-owned, survives reloads);
//     module members are rebuilt by onSimulationStart after every reload
//   - don't cache flecs queries/callbacks across frames unless you release
//     them in onDetach — they point into this library
#include <engine/game_module.h>
#include "runtime/runtime_context.h"
#include "runtime/logger.h"
#include "core/transform.h"
#include "components/mesh_renderer.h"
#include <flecs.h>
#include <bx/math.h>

// ── Edit me, rebuild, watch the running game change ─────────────────────────
static constexpr float       kSpinSpeed = 1.0f;  // radians/sec about Y
static constexpr const char* kBuildTag  = "v1";

class HotGame final : public IEnginePlugin {
public:
    const char* name()    const override { return "HotGame"; }
    const char* version() const override { return kBuildTag; }

    void onAttach(RuntimeContext&) override {
        LOG_SUCCESS("HotGame", "attached (%s) — spin %.2f rad/s",
                    kBuildTag, kSpinSpeed);
    }
    void onDetach() override {}

    void onSimulationStart(flecs::world& w) override {
        int n = 0;
        w.query_builder<Transform, const MeshRenderer>().build()
            .each([&](flecs::entity, Transform&, const MeshRenderer&) { ++n; });
        LOG_INFO("HotGame", "simulation start (%s) — spinning %d entit%s",
                 kBuildTag, n, n == 1 ? "y" : "ies");
    }
    void onSimulationStop() override {}

    void onUpdate(flecs::world& w, float dt) override {
        const bx::Quaternion spin =
            bx::fromAxisAngle({0.0f, 1.0f, 0.0f}, kSpinSpeed * dt);
        w.query_builder<Transform, const MeshRenderer>().build()
            .each([&](flecs::entity, Transform& t, const MeshRenderer&) {
                t.rotation = bx::normalize(bx::mul(spin, t.rotation));
            });
    }
};

ENGINE_GAME_MODULE(HotGame)
