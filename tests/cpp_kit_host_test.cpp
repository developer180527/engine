// cpp_kit_host_test — a C++ kit on eng.hpp, loaded from a project's manifest
// beside the C combat kit (WO-095).
//
// The regen kit (tests/fixtures/cpp_module_regen.cpp) is a shared library built
// with only include/eng/ on its path, no exceptions and no RTTI. At Play:
//   - it describes combat.Health itself, and the engine accepts that as the
//     C kit's own component;
//   - its system heals what the C kit's damage took, by its regen.Rate, up to
//     the max, and the change stream reports only the entities it healed;
//   - at Stop its RAII handles end what it made, and Play again heals once a
//     step, not twice.
#include "capi/eng_internal.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

#include "combat.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d: %s)\n", __FILE__, __LINE__, engGetLastErrorMessage()); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

namespace {

struct Rate { float per_step; uint32_t starts; };

fs::path makeProject() {
    const fs::path root = fs::temp_directory_path() / "engine_cpp_kit_host_test";
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    std::ofstream f(root / "project.json");
    f << "{\"name\":\"cppkits\",\"assetRoot\":\"assets\",\"kits\":["
      << "{\"name\":\"combat\",\"module\":\"" << C_KIT_COMBAT << "\"},"
      << "{\"name\":\"regen\",\"module\":\"" << CPP_KIT_REGEN << "\"}]}";
    return root;
}

void steps(EngineRuntime& rt, uint64_t n) {
    const uint64_t until = rt.simulationTicks() + n;
    for (int guard = 0; rt.simulationTicks() < until && guard < 10000; ++guard) rt.tickNoRender(1.0f / 60.0f);
}

float current(EngWorld* w, EngComponent health, EngEntity e) {
    CombatHealth h{};
    engGetComponent(w, e, health, &h, sizeof h);
    return h.current;
}

uint32_t changedSince(EngWorld* w, EngChangeStream s, EngEntity only, bool* onlyThat) {
    EngChange buf[64];
    uint32_t n = 64, changed = 0;
    *onlyThat = true;
    engReadChanges(w, s, buf, &n);
    for (uint32_t i = 0; i < n; ++i)
        if (buf[i].kind == ENG_CHANGE_CHANGED) { ++changed; *onlyThat = *onlyThat && buf[i].entity == only; }
    return changed;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cpp_kit_host_test: a C++ kit on eng.hpp beside a C one (WO-095)\n");
    const fs::path root = makeProject();

    EngineRuntime rt;
    EngineConfig cfg;
    cfg.projectRoot = root;
    cfg.defaultScene = false;
    cfg.openAssetDatabase = false;
    CHECK(rt.init(cfg, std::make_unique<HeadlessPlatform>()), "the engine");
    rt.attachPlugins();
    CHECK(rt.hasProject(), "the project opens");
    engInternalHostCModules(&rt);
    CHECK(rt.startSimulation(), "Play");
    CHECK(rt.kits().isLoaded("combat") && rt.kits().isLoaded("regen"),
          "the C kit and the C++ kit load from the manifest");
    EngWorld* w = engInternalAcquireKitWorld(&rt, rt.simWorld());

    const EngComponent health = engFindComponent(w, COMBAT_HEALTH);
    const EngComponent rate = engFindComponent(w, "regen.Rate");
    CHECK(health && rate, "combat.Health (the C kit's, as the C++ kit described it too) and regen.Rate");
    Rate seen{};
    engGetComponent(w, engFindEntity(w, "regen"), rate, &seen, sizeof seen);
    CHECK(seen.starts == 1, "the C++ kit's start ran once");

    EngEntity hurt = 0, whole = 0;
    engCreateEntity(w, "hurt", &hurt);
    engCreateEntity(w, "whole", &whole);
    const Rate two{2.0f, 0};
    for (EngEntity e : {hurt, whole}) {
        engAddComponent(w, e, health, nullptr);
        engAddComponent(w, e, rate, &two);
    }
    combatDealDamage(w, hurt, 30.0f);
    steps(rt, 1);
    const float afterHit = current(w, health, hurt);
    CHECK(afterHit == 70.0f || afterHit == 72.0f, "the C kit's damage landed: %g", afterHit);

    EngChangeStream s = 0;
    CHECK(engSubscribeChanges(w, health, &s) == ENG_OK, "a change stream on Health");
    EngChange backlog[64];
    uint32_t bn = 64;
    engReadChanges(w, s, backlog, &bn);
    const float before = current(w, health, hurt);
    steps(rt, 5);
    CHECK(current(w, health, hurt) == before + 10.0f,
          "the C++ kit's system heals 2 a step: %g after 5 steps from %g", current(w, health, hurt), before);
    bool onlyHurt = false;
    const uint32_t changed = changedSince(w, s, hurt, &onlyHurt);
    CHECK(changed == 1 && onlyHurt, "WriteMarked: only the healed entity is reported, not the whole one (%u)", changed);
    steps(rt, 20);
    CHECK(current(w, health, hurt) == 100.0f && current(w, health, whole) == 100.0f, "healed up to the max, no further");

    std::printf("\n-- Stop, and Play again --\n");
    engUnsubscribeChanges(w, s);
    engInternalReleaseKitWorld(w);
    rt.stopSimulation();
    CHECK(rt.startSimulation(), "Play again");
    w = engInternalAcquireKitWorld(&rt, rt.simWorld());
    engGetComponent(w, engFindEntity(w, "regen"), rate, &seen, sizeof seen);
    CHECK(seen.starts == 2, "the C++ kit started again");
    combatDealDamage(w, hurt, 40.0f);
    steps(rt, 1);
    const float b2 = current(w, health, hurt);
    steps(rt, 3);
    CHECK(current(w, health, hurt) == b2 + 6.0f,
          "its system ended at Stop with its handles: one heal a step, not two (%g from %g)",
          current(w, health, hurt), b2);

    std::printf("\n-- unloaded mid-play --\n");
    CHECK(rt.kitUnload("regen") && !rt.kits().isLoaded("regen"), "unloaded");
    const float b3 = current(w, health, hurt);
    steps(rt, 3);
    CHECK(current(w, health, hurt) == b3, "and no longer heals");

    engInternalReleaseKitWorld(w);
    rt.stopSimulation();
    rt.shutdown();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
