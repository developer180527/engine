// ── eng_step_test — engStep is the frame, and refuses by name (WO-070) ───────
//
//   1. Equivalence: the same physics scene, headless, driven 300 frames by
//      EngineRuntime's own frame loop (frameBegin / tick / frameEnd, as run()
//      does) and by engStepWithDelta alone. Same simulation tick count, same
//      world hash (simhash::hashWorld, what replays compare).
//   2. EngStepInfo adds up: the per-frame simulation_steps sum to the ticks.
//   3. Refusals, each by name (DR-0035): an unset or mismatched api_version,
//      an unknown provider kind or name, modules, a log callback, stepping
//      after quit, a window after a headless start, a stale window handle,
//      NULL arguments, and a struct written only up to its struct_size.
#include <eng/engine.h>

#include "capi/eng_internal.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "core/transform.h"
#include "plugins/stock_plugins.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/sim_hash.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static constexpr int    kFrames = 300;
static constexpr double kDt     = 1.0 / 60.0;

// A floor and a stack of boxes: enough for physics to move, collide and settle.
static void buildScene(flecs::world& w) {
    RigidBody floor{};
    floor.bodyType = PhysicsBodyType::Static;
    floor.halfExtent = {20, 0.5f, 20};
    w.entity("floor").set<Transform>({{0, -0.5f, 0}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(floor);
    for (int i = 0; i < 12; ++i) {
        RigidBody rb{};
        rb.bodyType = PhysicsBodyType::Dynamic;
        rb.halfExtent = {0.5f, 0.5f, 0.5f};
        rb.mass = 1;
        w.entity(("box" + std::to_string(i)).c_str())
            .set<Transform>({{(float)(i % 3) * 0.6f, 1.0f + (float)i * 1.1f, 0}, {0, 0, 0, 1}, {1, 1, 1}})
            .set<RigidBody>(rb);
    }
}

static EngEngineDesc desc() {
    EngEngineDesc d = ENG_INIT(EngEngineDesc);
    d.api_version = ENG_API_VERSION;
    d.app_name = "eng_step_test";
    return d;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("eng_step_test\n");

    // ── 1a. EngineRuntime's own loop ───────────────────────────────────────
    uint64_t ticksA = 0, hashA = 0;
    {
        EngineConfig cfg;
        cfg.autoDetectProject = false; cfg.defaultScene = false;
        cfg.openAssetDatabase = false; cfg.enableProfiler = false;
        EngineRuntime rt;
        CHECK(rt.init(cfg, std::make_unique<HeadlessPlatform>()), "EngineRuntime starts headless");
        addStockPlugins(rt);
        rt.attachPlugins();
        rt.startSimulation();
        buildScene(rt.simWorld());
        for (int f = 0; f < kFrames; ++f) {
            float ignored = 0;
            rt.frameBegin(ignored);   // run()'s body, with the test's clock
            rt.tick((float)kDt);
            rt.frameEnd();
        }
        ticksA = rt.simulationTicks();
        hashA = simhash::hashWorld(rt.simWorld());
        rt.stopSimulation();
        rt.shutdown();
    }

    // ── 1b. The same through engStepWithDelta ──────────────────────────────
    uint64_t ticksB = 0, hashB = 0, stepSum = 0, lastFrame = 0;
    uint32_t presented = 0;
    {
        EngEngineDesc d = desc();
        EngEngine* e = nullptr;
        CHECK(engCreateEngine(&d, &e) == ENG_OK && e, "engCreateEngine (code-first, no project)");
        EngineRuntime* rt = engInternalRuntime(e);
        CHECK(rt != nullptr, "the engine starts headless when no window was made");
        buildScene(rt->simWorld());
        for (int f = 0; f < kFrames; ++f) {
            EngStepInfo info = ENG_INIT(EngStepInfo);
            if (engStepWithDelta(e, kDt, &info) != ENG_OK) { CHECK(false, "engStepWithDelta: %s", engGetLastErrorMessage()); break; }
            stepSum += info.simulation_steps;
            lastFrame = info.frame;
            presented += info.windows_presented;
        }
        ticksB = rt->simulationTicks();
        hashB = simhash::hashWorld(rt->simWorld());
        CHECK(engIsRunning(e), "still running after %d steps", kFrames);
        engDestroyEngine(e);
    }
    CHECK(ticksA > 0 && ticksA == ticksB, "same simulation ticks: EngineRuntime %llu, engStep %llu",
          (unsigned long long)ticksA, (unsigned long long)ticksB);
    CHECK(hashA == hashB, "same world hash after %d frames (%016llx vs %016llx)", kFrames,
          (unsigned long long)hashA, (unsigned long long)hashB);
    CHECK(stepSum == ticksB, "EngStepInfo.simulation_steps sums to the ticks (%llu)", (unsigned long long)stepSum);
    CHECK(lastFrame == (uint64_t)kFrames, "EngStepInfo.frame counts frames (%llu)", (unsigned long long)lastFrame);
    CHECK(presented == 0, "headless: no window presented");

    // ── 2b. A code-first engine runs nothing from the working directory ────
    // It used to: with no project the script plugin's autorun folder resolved
    // against the cwd, so a program run from a folder with scripts/autorun/
    // ran those scripts (WO-070's manual check, from the repository root).
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "eng_step_test_cwd";
        fs::remove_all(dir);
        fs::create_directories(dir / "scripts" / "autorun");
        std::ofstream(dir / "scripts" / "autorun" / "marker.lua") << "print('autorun ran')\n";
        const fs::path was = fs::current_path();
        fs::current_path(dir);
        EngEngineDesc d = desc();
        EngEngine* e = nullptr;
        int scripts = -1;
        if (engCreateEngine(&d, &e) == ENG_OK) {
            engStepWithDelta(e, kDt, nullptr);
            scripts = 0;
            if (EngineRuntime* rt = engInternalRuntime(e))
                rt->simWorld().each([&](flecs::entity, const ScriptComponent&) { ++scripts; });
            engDestroyEngine(e);
        }
        fs::current_path(was);
        fs::remove_all(dir);
        CHECK(scripts == 0, "no script from the working directory runs without a project (%d)", scripts);
    }

    // ── 3. Refusals, each by name ──────────────────────────────────────────
    auto refused = [](EngResult got, EngResult want, const char* needle, const char* what) {
        const std::string msg = engGetLastErrorMessage();
        CHECK(got == want && msg.find(needle) != std::string::npos, "%s refused (%d): %s", what, got, msg.c_str());
    };
    EngEngine* e = nullptr;
    {
        EngEngineDesc d = desc(); d.api_version = 0;
        refused(engCreateEngine(&d, &e), ENG_ERROR_ABI_MISMATCH, "not set", "an unset api_version");
        d.api_version = ENG_API_VERSION + 1;
        refused(engCreateEngine(&d, &e), ENG_ERROR_ABI_MISMATCH, "newer", "a newer minor");
        d.api_version = (ENG_API_VERSION_MAJOR + 1) << 16;
        refused(engCreateEngine(&d, &e), ENG_ERROR_ABI_MISMATCH, "major", "another major");
    }
    {
        EngProviderChoice c[1] = {};
        c[0].kind = "physics"; c[0].name = "havok";
        EngEngineDesc d = desc(); d.providers = c; d.provider_count = 1;
        refused(engCreateEngine(&d, &e), ENG_ERROR_NOT_FOUND, "havok", "an unknown physics provider");
        c[0].kind = "renderer"; c[0].name = "aaa";
        refused(engCreateEngine(&d, &e), ENG_ERROR_NOT_FOUND, "renderer", "a provider kind this build cannot choose");
        c[0].kind = "physics"; c[0].name = "none";
        CHECK(engCreateEngine(&d, &e) == ENG_OK, "a known provider choice is accepted");
        engDestroyEngine(e); e = nullptr;
    }
    {
        EngEngineDesc d = desc(); d.module_count = 1;
        refused(engCreateEngine(&d, &e), ENG_ERROR_UNSUPPORTED, "modules", "a module list");
        d = desc(); d.log_fn = [](void*, EngLogLevel, const char*, const char*) {};
        refused(engCreateEngine(&d, &e), ENG_ERROR_UNSUPPORTED, "log_fn", "a log callback");
        d = desc(); d.project_path = "/no/such/project";
        refused(engCreateEngine(&d, &e), ENG_ERROR_NOT_FOUND, "project.json", "a missing project");
        refused(engCreateEngine(nullptr, &e), ENG_ERROR_INVALID_ARGUMENT, "NULL", "a NULL desc");
    }
    {
        EngEngineDesc d = desc();
        CHECK(engCreateEngine(&d, &e) == ENG_OK, "engCreateEngine");
        CHECK(engStepWithDelta(e, kDt, nullptr) == ENG_OK, "a step with out = NULL");
        EngWindowDesc wd = ENG_INIT(EngWindowDesc);
        EngWindow w = 0;
        refused(engCreateWindow(e, &wd, &w), ENG_ERROR_UNSUPPORTED, "before the first engStep",
                "a window after a headless start");
        refused(engSetWindowSize(e, 0x101, 10, 10), ENG_ERROR_STALE_HANDLE, "no longer exists", "a window that never was");
        refused(engStepWithDelta(e, -1.0, nullptr), ENG_ERROR_INVALID_ARGUMENT, "delta", "a negative delta");

        // A struct the engine fills is written only up to the caller's size.
        unsigned char raw[sizeof(EngStepInfo) + 8];
        std::memset(raw, 0xAB, sizeof raw);
        EngStepInfo* small = reinterpret_cast<EngStepInfo*>(raw);
        small->struct_size = 8;   // struct_size and simulation_steps only
        CHECK(engStepWithDelta(e, kDt, small) == ENG_OK, "a step into a short EngStepInfo");
        bool untouched = true;
        for (size_t i = 8; i < sizeof raw; ++i) untouched = untouched && raw[i] == 0xAB;
        CHECK(untouched && small->struct_size == 8, "nothing written past struct_size");

        engRequestQuit(e);
        CHECK(!engIsRunning(e), "engRequestQuit stops the run");
        refused(engStepWithDelta(e, kDt, nullptr), ENG_ERROR_REFUSED, "quit", "a step after quit");
        engDestroyEngine(e);
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
