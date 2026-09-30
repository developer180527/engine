// ── asset_cook_request_test — a missing cooked asset is a cook job (WO-018) ───
//
// The runtime loads cooked content only. An asset asked for by its SOURCE path
// with no Ready cook is:
//   * null provider (no cooker in this build): Failed at once, "not cooked";
//   * real/fake provider: Pending, a cook is requested, and it becomes Ready
//     when the cook lands (or Failed when the cook fails);
// and a scene entity waiting on it shows the placeholder, saves its AUTHORED
// reference, and swaps the real mesh in when it arrives.
//
// Before WO-018 the loader parsed the source itself (Assimp, cgltf) when
// nothing was cooked. This binary links engine_runtime and NOT the cook stack,
// so there is no parser in the process to call: `player_has_no_cook_stack`
// reads this binary's symbols too. The "cook" here is the fake provider
// writing a cooked file with assetlib and marking the registry record Ready,
// which is all a real cook publishes.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "gpu_test_device.h"

#include "assets/asset_storage.h"
#include "assets/cook_requests.h"
#include "components/mesh_placeholder.h"
#include "core/jobs/jobs.h"
#include "render/primitive_library.h"
#include "render/vertex.h"
#include "runtime/services/async_loader.h"
#include "runtime/services/scene_assets_host.h"
#include "scene/scene_serializer.h"
#include "scene/unresolved_mesh.h"
#include <assetlib/asset_registry.h>
#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>

namespace fs = std::filesystem;
using nlohmann::json;

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// The fake provider: records what was asked for. The test does the "cooking".
struct FakeCook final : ICookRequests {
    std::vector<std::string> asked;
    bool canCook() const override { return true; }
    void requestCook(const std::string& p) override { asked.push_back(p); }
};

struct Project {
    fs::path root;
    assetlib::AssetRegistry reg;

    explicit Project(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root / "assets");
        fs::create_directories(root / ".cache" / "meshs");
        reg.open(root / ".cache" / "registry.db");
    }
    ~Project() { reg.close(); fs::remove_all(root); }

    // A source file, registered the way the editor's scan registers it.
    fs::path source(const std::string& rel, const std::string& bytes = "not parsed") {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << bytes;
        reg.scan(root / "assets", root);
        return p;
    }
    assetlib::AssetRecord record(const fs::path& src) {
        return *reg.findBySourcePath(fs::relative(src, root).generic_string());
    }
    // What a cook publishes: a cooked file and a Ready record pointing at it.
    void publish(const fs::path& src, const std::function<bool(const fs::path&)>& write) {
        assetlib::AssetRecord r = record(src);
        const std::string rel = "meshs/" + r.uuid.toString() + ".cooked";
        write(root / ".cache" / rel);
        r.cookedPath = rel;
        r.state = assetlib::AssetState::Ready;
        reg.update(r);
    }
    void fail(const fs::path& src, const std::string& why) {
        assetlib::AssetRecord r = record(src);
        r.state = assetlib::AssetState::Failed;
        r.errorMessage = why;
        reg.update(r);
    }
};

// A one-triangle cooked mesh; `tex`, if set, is its base-colour texture as
// the source path (relative to the model) a cooked material stores.
static bool writeMesh(const fs::path& out, const char* tex = nullptr) {
    assetlib::MeshAsset m;
    m.header.vertexStride  = sizeof(Vertex);
    m.header.vertexCount   = 3;
    m.header.indexCount    = 3;
    m.header.indexStride   = 4;
    m.header.submeshCount  = 1;
    m.header.boundsMax[0]  = m.header.boundsMax[1] = m.header.boundsMax[2] = 1.0f;
    m.vertexData.assign(3 * sizeof(Vertex), 0);
    const uint32_t idx[3] = {0, 1, 2};
    m.indexData.assign(reinterpret_cast<const uint8_t*>(idx),
                       reinterpret_cast<const uint8_t*>(idx) + sizeof idx);
    assetlib::MeshSubmesh sub; sub.indexCount = 3;
    m.submeshes.push_back(sub);
    if (tex) {
        assetlib::CookedMaterial cm;
        cm.flags = assetlib::kMatFlag_HasBaseColor;
        std::snprintf(cm.baseColorPath, sizeof cm.baseColorPath, "%s", tex);
        m.materials.push_back(cm);
        m.header.materialCount = 1;
    }
    return assetlib::saveMesh(m, out);
}
static bool writeTexture(const fs::path& out) {
    assetlib::TextureAsset t;
    t.header.width = t.header.height = 2;
    t.header.format = assetlib::kTexRGBA8;
    t.pixels.assign(2 * 2 * 4, 0xFF);
    return assetlib::saveTexture(t, out);
}

// Drain until `done()` or the budget runs out. Parked requests retry every
// AsyncLoader::kRetryDrains drains, so this has to keep draining while idle.
static bool pump(AsyncLoader& l, AssetStorage& s, const std::function<bool()>& done,
                 int maxDrains = 4000) {
    for (int i = 0; i < maxDrains && !done(); ++i) {
        l.drainOne(s);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    return done();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("asset_cook_request_test — a missing cooked asset is a cook job\n");
    if (!initTestDevice()) return 1;
    jobs::init();
    {
    AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
    AssetStorage storage{meshes, textures, materials};

    // ── 1. Null provider: Failed at once, every caller told why ─────────────
    std::printf("1. no cooker in this build\n");
    {
        Project p("wo018_null");
        const fs::path src = p.source("assets/house.obj");
        AsyncLoader loader;
        loader.setRegistry(&p.reg); loader.setProjectRoot(p.root);
        int calls = 0; std::string err;
        auto cb = [&](const AsyncLoadResult& r, const std::string&) {
            ++calls; if (!r.mesh.valid()) err = r.error; };
        loader.load(src.string(), "a", cb);
        loader.load(src.string(), "b", cb);   // a waiter on the same path
        CHECK(pump(loader, storage, [&] { return calls == 2; }), "both callers heard back (%d)", calls);
        CHECK(err.find("not cooked") != std::string::npos, "the reason is \"not cooked\": %s", err.c_str());
        CHECK(loader.state(src.string()) == AssetLoadState::Failed, "state: Failed");
        NullCookRequests null;
        loader.setCookRequests(&null);
        calls = 0;
        loader.load(src.string(), "c", cb);
        CHECK(pump(loader, storage, [&] { return calls == 1; }) && err.find("not cooked") != std::string::npos,
              "NullCookRequests is the same answer");
    }

    // ── 2. Real/fake provider: Pending, cook requested, Ready when it lands ─
    std::printf("2. a cooker: Pending -> cook -> Ready\n");
    {
        Project p("wo018_job");
        const fs::path src = p.source("assets/house.obj");
        FakeCook cook;
        AsyncLoader loader;
        loader.setRegistry(&p.reg); loader.setProjectRoot(p.root);
        loader.setCookRequests(&cook);
        AsyncLoadResult got; int calls = 0;
        loader.load(src.string(), "house", [&](const AsyncLoadResult& r, const std::string&) { got = r; ++calls; });
        CHECK(pump(loader, storage, [&] { return !cook.asked.empty(); }), "a cook was requested");
        CHECK(cook.asked.size() == 1 && cook.asked[0] == src.string(), "for exactly this source");
        CHECK(loader.state(src.string()) == AssetLoadState::Pending, "state: Pending");
        pump(loader, storage, [] { return false; }, 100);   // retries happen; nothing is cooked
        CHECK(calls == 0, "no callback while Pending (it is not a failure)");
        CHECK(cook.asked.size() == 1, "and the cook is asked for once, not per retry (%zu)", cook.asked.size());

        p.publish(src, [](const fs::path& o) { return writeMesh(o); });
        CHECK(pump(loader, storage, [&] { return calls > 0; }), "the cook landed and the load finished");
        CHECK(calls == 1 && got.mesh.valid(), "with the mesh");
        CHECK(loader.state(src.string()) == AssetLoadState::Ready, "state: Ready");
    }

    // ── 3. The cook fails: Failed, with the cook's reason ──────────────────
    std::printf("3. the cook fails\n");
    {
        Project p("wo018_fail");
        const fs::path src = p.source("assets/broken.obj");
        FakeCook cook;
        AsyncLoader loader;
        loader.setRegistry(&p.reg); loader.setProjectRoot(p.root);
        loader.setCookRequests(&cook);
        AsyncLoadResult got; int calls = 0;
        loader.load(src.string(), "broken", [&](const AsyncLoadResult& r, const std::string&) { got = r; ++calls; });
        pump(loader, storage, [&] { return !cook.asked.empty(); });
        p.fail(src, "no triangles");
        CHECK(pump(loader, storage, [&] { return calls > 0; }), "the failure reached the caller");
        CHECK(!got.mesh.valid() && got.error.find("cook failed") != std::string::npos
              && got.error.find("no triangles") != std::string::npos,
              "as \"cook failed\", with the cook's reason: %s", got.error.c_str());
        CHECK(loader.state(src.string()) == AssetLoadState::Failed, "state: Failed");
    }

    // ── 4. A cooked mesh waits for its uncooked texture ────────────────────
    // Otherwise it would appear untextured and change later: the same asset
    // looking different depending on cook order, which is what WO-018 ends.
    std::printf("4. a cooked mesh waits for its texture\n");
    {
        Project p("wo018_tex");
        const fs::path tex = p.source("assets/house_albedo.png");
        const fs::path src = p.source("assets/house.obj");
        p.publish(src, [](const fs::path& o) { return writeMesh(o, "house_albedo.png"); });
        FakeCook cook;
        AsyncLoader loader;
        loader.setRegistry(&p.reg); loader.setProjectRoot(p.root);
        loader.setCookRequests(&cook);
        AsyncLoadResult got; int calls = 0;
        loader.load(src.string(), "house", [&](const AsyncLoadResult& r, const std::string&) { got = r; ++calls; });
        CHECK(pump(loader, storage, [&] { return !cook.asked.empty(); }), "a cook was requested");
        CHECK(cook.asked.size() == 1 && fs::path(cook.asked[0]).filename() == "house_albedo.png",
              "for the TEXTURE (the mesh is cooked): %s", cook.asked.empty() ? "-" : cook.asked[0].c_str());
        CHECK(calls == 0 && loader.state(src.string()) == AssetLoadState::Pending, "the mesh is Pending meanwhile");
        p.publish(tex, [](const fs::path& o) { return writeTexture(o); });
        CHECK(pump(loader, storage, [&] { return calls > 0; }) && got.mesh.valid(), "then it loads");
        const Mesh* m = meshes.getMesh(got.mesh);
        const Material* mat = m ? materials.getMaterial(m->material) : nullptr;
        CHECK(mat && mat->hasTexture(), "textured");
    }

    // ── 5. A scene entity: placeholder, authored reference saved, swap ─────
    std::printf("5. a scene entity waiting on a cook\n");
    {
        Project p("wo018_scene");
        const fs::path src = p.source("assets/house.obj");
        PrimitiveLibrary prims; prims.init(meshes);
        FakeCook cook;
        AsyncLoader loader;
        loader.setRegistry(&p.reg); loader.setProjectRoot(p.root);
        loader.setCookRequests(&cook);

        const json authored = {{"path", "assets/house.obj"}};
        json scene = {{"version", 1}, {"entities", json::array({
            {{"name", "house"}, {"transform", {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}},
                                               {"scale", {1, 1, 1}}}},
             {"meshRenderer", authored}}})}};
        const fs::path scenePath = p.root / "scenes" / "main.scene";
        fs::create_directories(scenePath.parent_path());
        std::ofstream(scenePath) << scene.dump();

        flecs::world w;
        CHECK(SceneSerializer::loadAsync(scenePath, w, storage, sceneAssetsFor(nullptr, &loader),
                                         &prims, p.root), "scene loaded");
        flecs::entity e;
        w.each([&](flecs::entity x, const Name& n) { if (n.value == "house") e = x; });
        const MeshRenderer* mr = e ? e.try_get<MeshRenderer>() : nullptr;
        CHECK(mr && mr->mesh == prims.cube() && e.has<MeshPlaceholder>(),
              "it shows the placeholder cube, not nothing");

        const fs::path saved = p.root / "scenes" / "saved.scene";
        SceneSerializer::save(saved, w, meshes, &p.reg, p.root);
        auto meshOf = [&] {
            std::ifstream f(saved); const json s = json::parse(f);
            return s["entities"][0].value("meshRenderer", json::object()); };
        CHECK(meshOf() == authored, "a save while it cooks writes the authored reference, not the cube: %s",
              meshOf().dump().c_str());

        CHECK(pump(loader, storage, [&] { return !cook.asked.empty(); }), "a cook was requested");
        p.publish(src, [](const fs::path& o) { return writeMesh(o); });
        CHECK(pump(loader, storage, [&] { return !e.has<MeshPlaceholder>(); }), "the cook landed");
        mr = e.try_get<MeshRenderer>();
        CHECK(mr && mr->mesh != prims.cube() && meshes.getMesh(mr->mesh), "the real mesh swapped in");
        CHECK(!e.has<UnresolvedMesh>(), "and the reference is resolved");
        SceneSerializer::save(saved, w, meshes, &p.reg, p.root);
        CHECK(meshOf().value("path", std::string{}) == "assets/house.obj",
              "a save now writes the real mesh's reference: %s", meshOf().dump().c_str());
    }
    }
    jobs::shutdown();
    shutdownTestDevice();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
