// ── cooked_scene_test — what the shipped player gets is what was authored ─────
//
// WO-091. The shipped player loads `scenes/<name>.cooked` through SceneService.
// Until WO-091 that file was a fixed 256-byte record per entity with slots for
// nine components, so colour grading, kits' components and anything unknown
// never reached a shipped game. Now the cooked scene is the scene's own saved
// form, read through the schema registry like the editor reads a .scene.
//
// The test is the round trip that matters: the real cooker, the real loader (on
// bgfx's Noop backend), then the world saved back with the editor's serializer
// and compared with the source, entity by entity. The skin wiring a cooked
// skinned mesh gets (skeleton and clip) is checked with a fake mesh in
// scene_schema_test, where the handles can be chosen.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "gpu_test_device.h"
#include "assets/cookers/scene/scene_cooker.h"
#include "components/meta_registry.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"
#include "scene/entity_serializer.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

namespace {
struct Health { float current = 100.0f; float max = 100.0f; };
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cooked_scene_test: the shipped player gets every component (WO-091)\n");
    if (!initTestDevice()) return 1;

    const fs::path root = fs::temp_directory_path() / "engine_cooked_scene_test";
    fs::remove_all(root);
    fs::create_directories(root / ".cache" / "scenes");

    // The source: every engine component (the WO-071 fixture), and on top of it
    // the shapes only the registry carries.
    json source;
    {
        std::ifstream f(std::string(ENGINE_SOURCE_DIR) + "/tests/fixtures/scenes/every_component.disk.json");
        source = json::parse(f);
        json& first = source["entities"][0];
        first["sprite"] = { {"atlas", "ui/icons.png"}, {"frame", 3} };      // nothing registers it
        first["reflected"]["armour::Plate"] = { {"rating", 7} };            // a kit not loaded
        source["entities"].push_back({ {"id", 1003}, {"name", "Future"},
            {"camera", { {"$rev", 9}, {"fov", 10.0} }} });                   // a newer revision
    }
    const fs::path src = root / "main.scene";
    { std::ofstream f(src); f << source.dump(2); }
    const fs::path cooked = root / ".cache" / "scenes" / "main.cooked";
    CHECK(cookSceneFile(src, cooked, nullptr, root), "the scene cooks");

    flecs::world world;
    MetaRegistry::registerAll(world);
    world.component<Health>("combat::Health")
        .member("current", &Health::current).member("max", &Health::max);
    AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
    AssetService assets({meshes, textures, materials});
    assets.setProjectRoot(root);
    {
        SceneService svc({assets, meshes, textures, materials, world});
        svc.setCacheRoot(root / ".cache");
        const uint32_t h = svc.loadScene("scenes/main.cooked");
        CHECK(h != 0 && svc.sceneEntityCount(h) == 3,
              "SceneService loads it: 3 entities (%u)", h ? svc.sceneEntityCount(h) : 0);
    }

    std::printf("\n-- each component on its entity --\n");
    const flecs::entity a = findById(world, 1001), b = findById(world, 1002),
                        f = findById(world, 1003);
    const ColourGrading* g = a.try_get<ColourGrading>();
    CHECK(g && g->exposureEV == -1.5f && g->toneMapper == 2 && g->lutPath == "assets/luts/warm.cube",
          "colour grading reaches the game (exposure, tone mapper, LUT)");
    const Health* hp = a.try_get<Health>();
    CHECK(hp && hp->current == 40.0f && hp->max == 120.0f, "a kit's component reaches the game");
    CHECK(a.try_get<Light>() && a.try_get<Light>()->type == LightType::Spot
          && a.try_get<RigidBody>() && a.try_get<CharacterController>()
          && a.try_get<ScriptComponent>() && a.try_get<Camera>(),
          "and the nine components the old record held still do");
    CHECK(b.try_get<Animator>() && b.try_get<Animator>()->clipIndex == 3
          && b.target(flecs::ChildOf) == a, "the child keeps its animator and its parent");
    CHECK(!f.has<Camera>(), "a camera from a newer revision is not loaded as a default one");

    std::printf("\n-- saved back, it is the source --\n");
    EntitySerde::SerdeContext ctx; ctx.mode = EntitySerde::SerdeMode::Disk;
    for (std::size_t i = 0; i < source["entities"].size(); ++i) {
        const json& want = source["entities"][i];
        const flecs::entity e = findById(world, want["id"].get<uint64_t>());
        const json got = EntitySerde::saveEntity(e, ctx);
        CHECK(got == want, "entity %llu saves back exactly as authored%s",
              (unsigned long long)want["id"].get<uint64_t>(),
              got == want ? "" : (" — got " + got.dump()).c_str());
    }

    shutdownTestDevice();
    fs::remove_all(root);
    if (g_failures) {
        std::printf("\ncooked_scene_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncooked_scene_test: all checks passed\n");
    return 0;
}
