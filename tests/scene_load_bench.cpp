// ── scene_load_bench — what loading a cooked scene costs, in numbers (WO-091) ──
//
// Not a pass/fail test. A 50 000-entity scene, cooked with the real cooker and
// loaded through the real SceneService (the shipped player's path), on bgfx's
// Noop backend. Only the public cook and load calls are used, so the same file
// measures any cooked format: WO-091 replaced the fixed 256-byte record with the
// schema registry's form and this is the before/after.
//
//   cmake --build build-rel --target scene_load_bench && build-rel/scene_load_bench
//
// The scene: every entity a name and transform, one in four parented to an
// earlier one, one in two a mesh (one shared cooked mesh, so it is the scene's
// cost and not the mesh's), one in ten a light, one in five a rigid body.
// Each line is the median of 5 loads into a fresh world.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <assetlib/mesh_asset.h>
#include <flecs.h>
#include <nlohmann/json.hpp>

#include "gpu_test_device.h"
#include "assets/cookers/scene/scene_cooker.h"
#include "components/entity_id.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "render/vertex.h"
#include "runtime/services/asset_service.h"
#include "runtime/services/scene_service.h"
#include "assets/cooked_scene.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool writeMesh(const fs::path& out) {
    assetlib::MeshAsset m;
    m.header.version      = 4;
    m.header.vertexFlags  = assetlib::VF_POSITION | assetlib::VF_NORMAL
                          | assetlib::VF_UV0 | assetlib::VF_TANGENT;
    m.header.vertexStride = sizeof(Vertex);
    m.header.vertexCount  = 3;
    m.header.indexStride  = 4;
    m.header.indexCount   = 3;
    m.vertexData.assign((size_t)m.header.vertexCount * m.header.vertexStride, 0);
    const uint32_t idx[3] = { 0, 1, 2 };
    m.indexData.assign((const uint8_t*)idx, (const uint8_t*)idx + sizeof(idx));
    for (int i = 0; i < 3; ++i) { m.header.boundsMin[i] = -1.0f; m.header.boundsMax[i] = 1.0f; }
    return assetlib::saveMesh(m, out);
}

// The scene as the editor saves it (the keys and shapes of entity_serializer.h).
json makeScene(uint32_t n) {
    json scene;
    scene["version"]  = 1;
    scene["entities"] = json::array();
    for (uint32_t i = 0; i < n; ++i) {
        json e;
        e["id"]   = (uint64_t)(i + 1);
        e["name"] = "Entity " + std::to_string(i);
        if (i % 4 == 3) e["parentId"] = (uint64_t)(i / 2 + 1);
        e["transform"] = { {"position", {float(i % 100), 0.0f, float(i / 100)}},
                           {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}},
                           {"scale",    {1.0f, 1.0f, 1.0f}} };
        if (i % 2 == 0)
            e["meshRenderer"] = { {"path", "assets/models/prop.glb"},
                                  {"cookedPath", "meshs/prop.cooked"} };
        if (i % 10 == 0)
            e["light"] = { {"type", 1}, {"color", {1.0f, 0.9f, 0.8f}}, {"intensity", 4.0f},
                           {"range", 10.0f}, {"spotInner", 25.0f}, {"spotOuter", 35.0f},
                           {"castShadows", false}, {"useTemperature", false},
                           {"temperatureK", 6500.0f} };
        if (i % 5 == 0)
            e["rigidBody"] = { {"bodyType", 2}, {"shape", 0}, {"mass", 1.0f},
                               {"restitution", 0.3f}, {"friction", 0.6f},
                               {"useGravity", true}, {"halfExtent", {0.5f, 0.5f, 0.5f}},
                               {"radius", 0.5f}, {"halfHeight", 0.5f} };
        scene["entities"].push_back(std::move(e));
    }
    return scene;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // --profile: the 50 000-entity load only, 40 times, for a sampling profiler.
    const bool profile = argc > 1 && std::string(argv[1]) == "--profile";
    const fs::path root = fs::temp_directory_path() / "engine_scene_load_bench";
    fs::remove_all(root);
    fs::create_directories(root / ".cache" / "meshs");
    fs::create_directories(root / ".cache" / "scenes");
    if (!writeMesh(root / ".cache" / "meshs" / "prop.cooked")) return 1;
    if (!initTestDevice()) return 1;

    for (uint32_t n : profile ? std::vector<uint32_t>{50000u} : std::vector<uint32_t>{5000u, 50000u}) {
        const fs::path src = root / ("bench" + std::to_string(n) + ".scene");
        { std::ofstream f(src); f << makeScene(n).dump(2); }
        const fs::path cooked = root / ".cache" / "scenes" / ("bench" + std::to_string(n) + ".cooked");
        const auto c0 = std::chrono::steady_clock::now();
        if (!cookSceneFile(src, cooked, nullptr, root)) return 1;
        const double cookMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - c0).count();

        std::vector<double> ms;
        uint32_t entities = 0;
        for (int run = 0; run < (profile ? 40 : 5); ++run) {
            flecs::world world;
            AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
            AssetService assets({meshes, textures, materials});
            assets.setProjectRoot(root);
            SceneService svc({assets, meshes, textures, materials, world});
            svc.setCacheRoot(root / ".cache");
            const std::string rel = "scenes/bench" + std::to_string(n) + ".cooked";
            const auto t0 = std::chrono::steady_clock::now();
            const uint32_t h = svc.loadScene(rel.c_str());
            ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count());
            entities = h ? svc.sceneEntityCount(h) : 0;
            if (h) svc.unloadScene(h);
            bgfx::frame();
        }
        // What decoding the payload into a JSON tree alone costs: the tree path a
        // load falls back to for bytes the cook does not write, and what every
        // load paid before WO-092 read the CBOR in place.
        std::vector<double> dec;
        for (int run = 0; run < (profile ? 1 : 5); ++run) {
            assetlib::SceneAsset file; json scene; std::string why;
            const auto t0 = std::chrono::steady_clock::now();
            if (assetlib::loadScene(file, cooked, &why)) cooked_scene::decode(file, scene, why);
            dec.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(dec.begin(), dec.end());
        std::sort(ms.begin(), ms.end());
        std::printf("entities %6u  loaded %6u  cooked %8.1f KB  cook %8.1f ms  load %8.1f ms"
                    "  (a JSON tree alone: %6.1f ms)\n",
                    n, entities, fs::file_size(cooked) / 1024.0, cookMs, ms[ms.size() / 2],
                    dec[dec.size() / 2]);
    }
    shutdownTestDevice();
    fs::remove_all(root);
    return 0;
}
