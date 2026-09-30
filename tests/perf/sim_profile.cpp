// ── sim_profile — where a SIMULATION tick's time goes, with no GPU ────────────
//
// The renderer will be replaced, so its cost is not what to tune now. This
// measures the part that stays: the fixed step (intent, script update, moves,
// physics, animation, ECS progress, ...) on a headless runtime, at a scale a
// real scene reaches, and reports every profiler scope's cost per tick.
//
//   sim_profile [scale] [ticks]     scale 1 = the world below; 2 doubles it
//
// The world (scale 1):
//   5 000 props (1 000 spinning), 200 spinning parents with 4 children each
//   physics: a floor, 1 000 dynamic boxes dropped into piles so they collide,
//            32 character controllers
//   animation: 100 skinned entities, a 60-bone hierarchy, a 2 s clip keyed on
//              every joint (translation + rotation, 30 keys each)
//   scripting: 200 Lua entities reading and writing their transform each tick
//
// PERF lane, never a gate: absolute numbers are the machine's, not the repo's.
// Build it OPTIMISED with the profiler on, or it measures -O0:
//   cmake -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-DENGINE_PROFILE=1
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <flecs.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>

#include "animation/ozz_bridge.h"
#include "components/animator.h"
#include "components/character_controller.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "components/skinned_mesh.h"
#include "components/spinner.h"
#include "core/profiler.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

namespace fs = std::filesystem;
static constexpr float kSimDt = 1.0f / 60.0f;

static Skeleton makeSkeleton(int bones) {
    Skeleton s;
    s.bones.resize((size_t)bones);
    for (int i = 0; i < bones; ++i) {                 // a tree: spine chain with limbs off it
        s.bones[(size_t)i].name        = "b" + std::to_string(i);
        s.bones[(size_t)i].parentIndex = i == 0 ? -1 : (i < 8 ? i - 1 : (i % 8));
    }
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);
    return s;
}

static AnimClip makeClip(const Skeleton& skel, float duration, int keys) {
    ozz::animation::offline::RawAnimation raw;
    raw.duration = duration;
    raw.tracks.resize((size_t)skel.ozz->num_joints());
    for (size_t j = 0; j < raw.tracks.size(); ++j)
        for (int k = 0; k < keys; ++k) {
            const float t = duration * (float)k / (float)(keys - 1), a = 0.3f * std::sin(t * 3.0f + (float)j);
            raw.tracks[j].translations.push_back({t, {0.0f, 0.1f * (float)j, 0.0f}});
            raw.tracks[j].rotations.push_back({t, {std::sin(a / 2), 0.0f, 0.0f, std::cos(a / 2)}});
        }
    AnimClip clip = anim::finishOzzClip(raw, skel, "profile_clip", (int)raw.tracks.size(), (int)raw.tracks.size());
    return clip;
}

static Transform at(float x, float y, float z) { return {{x, y, z}, {0, 0, 0, 1}, {1, 1, 1}}; }

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int scale = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
    const int ticks = argc > 2 ? std::max(10, std::atoi(argv[2])) : 600;
    std::printf("sim_profile: scale %d, %d measured ticks%s\n", scale, ticks,
#if ENGINE_PROFILE
                ""
#else
                "  (PROFILER COMPILED OUT: rebuild with -DENGINE_PROFILE=1)"
#endif
                );

    const fs::path root = fs::temp_directory_path() / "engine_sim_profile";
    fs::remove_all(root);
    fs::create_directories(root / "scripts");
    { std::ofstream(root / "scripts" / "mover.lua") <<
        "local M = {}\n"
        "function M:onStart() self.t = 0 end\n"
        "function M:onUpdate(dt)\n"
        "  self.t = self.t + dt\n"
        "  local tr = self.entity:getTransform()\n"
        "  tr.position.y = 1 + math.sin(self.t)\n"
        "  self.entity:setTransform(tr)\n"
        "end\n"
        "return M\n"; }

    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.enableProfiler    = true;
    cfg.projectRoot       = root;
    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) { std::printf("init failed\n"); return 1; }
    engine.plugins().add(std::make_shared<JoltPlugin>());
    engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    engine.attachPlugins();
    flecs::world& w = engine.simWorld();

    // ── The world ───────────────────────────────────────────────────────────
    int n = 0;
    for (int i = 0; i < 5000 * scale; ++i, ++n) {
        auto e = w.entity().set<Transform>(at((float)(i % 100), 0.0f, (float)(i / 100)));
        if (i % 5 == 0) e.set<Spinner>({0.5f + 0.001f * (float)i, 0.3f});
    }
    for (int p = 0; p < 200 * scale; ++p) {
        auto parent = w.entity().set<Transform>(at((float)p, 3.0f, -5.0f)).set<Spinner>({0.4f, 0.2f});
        for (int c = 0; c < 4; ++c) w.entity().set<Transform>(at(0.5f * (float)c, 0.5f, 0.0f)).set<Spinner>({0.7f, 0.3f}).child_of(parent);
        n += 5;
    }
    RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static; floor.halfExtent = {200.0f, 0.5f, 200.0f};
    w.entity().set<Transform>(at(0, -0.5f, 0)).set<RigidBody>(floor);
    for (int i = 0; i < 1000 * scale; ++i, ++n) {
        RigidBody rb{}; rb.bodyType = PhysicsBodyType::Dynamic; rb.halfExtent = {0.4f, 0.4f, 0.4f}; rb.mass = 1.0f;
        const int pile = i / 50, k = i % 50;          // piles of 50, so boxes rest on boxes
        w.entity().set<Transform>(at((float)(pile % 10) * 6.0f + (float)(k % 5) * 0.85f, 0.5f + (float)(k / 25) * 0.9f + (float)(k % 25 / 5) * 0.9f,
                                     (float)(pile / 10) * 6.0f)).set<RigidBody>(rb);
    }
    for (int i = 0; i < 32 * scale; ++i, ++n) {
        CharacterController cc{};
        w.entity().set<Transform>(at(-20.0f - (float)(i % 8) * 1.5f, 1.5f, (float)(i / 8) * 1.5f)).set<CharacterController>(cc);
    }
    const Skeleton skel = makeSkeleton(60);
    const AnimClip clip = makeClip(skel, 2.0f, 30);
    for (int i = 0; i < 100 * scale; ++i, ++n) {
        SkinnedMesh sm{}; sm.skeleton = engine.skeletons().add(Skeleton(skel));
        Animator a{}; a.clip = engine.clips().add(AnimClip(clip)); a.playing = true; a.looping = true;
        a.time = 0.017f * (float)i;
        w.entity().set<Transform>(at((float)i, 0, 30)).set<SkinnedMesh>(sm).set<Animator>(a);
    }
    for (int i = 0; i < 200 * scale; ++i, ++n)
        w.entity().set<Transform>(at((float)i, 1, 40)).set<Name>({"scripted_" + std::to_string(i)})
                  .set<ScriptComponent>({"scripts/mover.lua"});
    std::printf("        %d entities; 60-bone rigs; %d bodies\n", n, 1000 * scale + 32 * scale);

    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    // ── Run: warm up, then measure ──────────────────────────────────────────
    // frameBegin is called for the profiler's frame boundaries only: it writes
    // the WALL-CLOCK frame delta into its argument, and this loop runs far
    // faster than real time. tick gets exactly kSimDt, so every measured frame
    // is exactly one fixed step.
    auto frame = [&] { float wall = 0; engine.frameBegin(wall); engine.tick(kSimDt); engine.frameEnd(); };
    for (int i = 0; i < 60; ++i) frame();

    std::map<std::string, std::vector<double>> perTick;   // scope -> microseconds per tick (main thread)
    std::map<std::string, double> workerTotal;            // scope -> microseconds summed over worker threads
    std::vector<double> tickUs;
    for (int t = 0; t < ticks; ++t) {
        frame();
        std::map<std::string, double> thisTick;
        double top = 0;
        for (const prof::TimerSample& s : prof::Profiler::get().timer().lastFrame()) {
            const double us = (double)(s.end - s.start) / 1000.0;
            if (s.threadIndex == 0) {
                thisTick[s.name] += us;
                if (s.depth == 0) top += us;
            } else {
                workerTotal[s.name] += us;
            }
        }
        for (auto& [k, v] : thisTick) perTick[k].push_back(v);
        tickUs.push_back(top);
    }
    engine.stopSimulation();

    auto pct = [](std::vector<double> v, double q) { std::sort(v.begin(), v.end()); return v[(size_t)((double)(v.size() - 1) * q)]; };
    double mean = 0; for (double v : tickUs) mean += v; mean /= (double)tickUs.size();
    std::printf("\n  frame (sum of top-level scopes, main thread): mean %.1f us, p50 %.1f, p95 %.1f, max %.1f   (budget at 60 Hz: 16667 us)\n\n",
                mean, pct(tickUs, 0.5), pct(tickUs, 0.95), pct(tickUs, 1.0));
    std::vector<std::pair<double, std::string>> rows;
    for (auto& [k, v] : perTick) { double m = 0; for (double x : v) m += x; rows.push_back({m / (double)ticks, k}); }
    std::sort(rows.rbegin(), rows.rend());
    std::printf("  %-28s %10s %10s %10s %10s %7s\n", "scope (main thread)", "mean us", "p50", "p95", "max", "% frame");
    for (auto& [m, k] : rows) {
        auto v = perTick[k];
        while (v.size() < (size_t)ticks) v.push_back(0.0);   // ticks where the scope did not run
        std::printf("  %-28s %10.1f %10.1f %10.1f %10.1f %6.1f%%\n", k.c_str(), m, pct(v, 0.5), pct(v, 0.95), pct(v, 1.0), 100.0 * m / mean);
    }
    if (!workerTotal.empty()) {
        std::printf("\n  %-28s %10s\n", "scope (worker threads)", "us/tick");
        std::vector<std::pair<double, std::string>> wr;
        for (auto& [k, v] : workerTotal) wr.push_back({v / (double)ticks, k});
        std::sort(wr.rbegin(), wr.rend());
        for (auto& [m, k] : wr) std::printf("  %-28s %10.1f\n", k.c_str(), m);
    }
    engine.shutdown();
    fs::remove_all(root);
    return 0;
}
