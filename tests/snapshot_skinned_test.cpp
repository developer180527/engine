// ── snapshot_skinned_test — Snapshot Play with a skinned entity in the scene ─
//
// Found while fixing BUG-0061 (the per-entity animation context leak). The
// animator's palette-release hook is installed LAZILY, the first time
// AnimatorSystem::run() sees a world. For the Snapshot play world that is the
// first fixed step — AFTER startSimulation has already filled the world with
// SceneSerializer::loadIntoWorld, and loadSkinnedMesh puts SkinnedMesh on every
// skinned entity in the scene. flecs refuses to set hooks on a component that
// is already in use (type_info.c: ALREADY_IN_USE) and aborts.
//
// So the prediction this file tests: Snapshot Play of ANY scene containing a
// skinned entity aborts on its first fixed step, in a build with flecs asserts
// on. InPlace Play never calls run() — it animates the init world, hooked in
// init() — and the determinism gate runs InPlace, which is why nothing caught
// it. This drives the Snapshot path end to end.
//
// Confirmed 2026-09-13: before the fix this exits 134 on the first tick with
// flecs' ALREADY_IN_USE assert. Fixed by AnimatorSystem::prepareWorld, which
// startSimulation calls before loading the snapshot (BUG-0062).
#include <cstdio>
#include <filesystem>
#include <memory>

#include <flecs.h>

#include "components/name.h"
#include "components/skinned_mesh.h"
#include "core/transform.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static const std::filesystem::path& hermeticRoot() {
    static const std::filesystem::path p = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path()
                                / "engine_snapshot_skinned_root";
        std::error_code ec;
        std::filesystem::remove_all(d, ec);
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("snapshot_skinned_test — Snapshot Play with a skinned entity\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;
    cfg.defaultScene      = false;
    cfg.projectRoot       = hermeticRoot();

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("  FAIL  engine init\n");
        return 1;
    }
    engine.attachPlugins();

    // The EDIT world, before Play. Name + Transform so the scene serializer
    // carries it into the snapshot.
    flecs::world& edit = engine.simWorld();
    edit.entity().set<Transform>({{0.f,0.f,0.f},{0,0,0,1},{1,1,1}})
                 .set<Name>({"skinned"})
                 .set<SkinnedMesh>(SkinnedMesh{});

    CHECK(engine.startSimulation(EngineRuntime::SimMode::Snapshot),
          "Snapshot play starts");

    // Non-vacuity: if the snapshot dropped the entity, the hook would be set on
    // an unused component and this test would prove nothing.
    int skinned = 0;
    engine.simWorld().each([&](flecs::entity, const SkinnedMesh&) { ++skinned; });
    CHECK(skinned == 1,
          "the snapshot world carries the skinned entity (%d) — without it the "
          "hook would be installed on an unused component and nothing here "
          "could fail", skinned);

    for (int i = 0; i < 3; ++i) engine.tick(1.0f / 60.0f);
    CHECK(true, "three fixed steps of Snapshot play ran without aborting");

    // And it must survive for the RIGHT reason. run()'s fallback also avoids
    // the abort — by NOT installing the hook, which leaks an 8 KB palette per
    // skinned entity per death. The correct path installs it before the
    // snapshot fills the world, and only that path leaves it on the play world.
    {
        flecs::world& play = engine.simWorld();
        const ecs_type_info_t* ti =
            ecs_get_type_info(play.c_ptr(), play.component<SkinnedMesh>().id());
        CHECK(ti && ti->hooks.on_remove,
              "the play world carries the SkinnedMesh release hook, installed "
              "before the snapshot was loaded into it");
    }

    engine.stopSimulation();
    engine.shutdown();

    if (g_failures) {
        std::printf("\nsnapshot_skinned_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nsnapshot_skinned_test: ALL PASS\n");
    return 0;
}
