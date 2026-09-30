// ── scene_material_roundtrip_test — a material override survives save + load ──
//
// BUG-0072. SceneSerializer::save installed the handle -> name lookup only
// `if (ctx.assetService)`, testing a field of the context it had built three
// lines up and never set. So the lookup was never installed, no scene file
// ever got a "material" key, and every override reopened as the mesh's own
// material. A mesh streamed on a worker lost its override at load as well: the
// completion callback set MeshRenderer{mesh} with no material.
//
// Goes through the real SceneSerializer::save / loadAsync with a FAKE
// SceneAssets (WO-047): the hooks are the whole seam, so no GPU, no runtime, no
// asset files beyond an empty placeholder the source path must point at.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "scene/scene_serializer.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

namespace fs = std::filesystem;
using nlohmann::json;

// The host, faked: two named materials, cooked meshes that always load (with
// one LOD level), and a streamer that completes at once.
struct FakeHost {
    AssetRegistry    meshes;
    TextureRegistry  textures;
    MaterialRegistry materials;
    AssetStorage     storage{meshes, textures, materials};
    int streamed = 0;

    MaterialHandle brick{1}, glass{2};
    MeshHandle addMesh(const std::string& source) {
        Mesh m; m.sourcePath = source; return meshes.addMesh(std::move(m));
    }
    SceneAssets hooks(bool withMaterials = true, bool canStream = true) {
        SceneAssets a;
        a.loadCookedMesh = [this](const char*, std::vector<MeshHandle>* lods) {
            if (lods) lods->push_back(addMesh("lod1"));
            return addMesh("assets/models/house.fbx");
        };
        if (withMaterials) {
            a.loadMaterial = [this](const char* n) {
                return std::string(n) == "Brick" ? brick
                     : std::string(n) == "Glass" ? glass : MaterialHandle{};
            };
            a.materialName = [this](MaterialHandle h) {
                return h == brick ? std::string("Brick") : h == glass ? std::string("Glass")
                                                                      : std::string{};
            };
        }
        if (canStream)
            a.streamMesh = [this](const std::string& path, const std::string&,
                                  std::function<void(const StreamedMesh&)> done) {
                ++streamed;
                done({addMesh(path), {}, {}});
            };
        return a;
    }
};

static flecs::entity spawn(flecs::world& w, const char* name, MeshHandle mesh, MaterialHandle mat) {
    return w.entity(name).set<Name>({name}).set<Transform>({})
            .set<MeshRenderer>({mesh, mat});
}
static flecs::entity byName(flecs::world& w, const std::string& n) {
    flecs::entity found;
    w.each([&](flecs::entity e, const Name& nm) { if (nm.value == n) found = e; });
    return found;
}
static json readJson(const fs::path& p) { std::ifstream f(p); return json::parse(f); }
// An entity's saved meshRenderer, or {} when absent. The fake meshes have no
// GPU buffers, so the registry reports them missing and no path is written —
// the material key does not depend on it, and it is all this test reads.
static json meshOf(const json& scene, const std::string& n) {
    for (const auto& e : scene["entities"])
        if (e.value("name", std::string{}) == n) {
            auto it = e.find("meshRenderer");   // written as null when empty
            return it != e.end() && it->is_object() ? *it : json::object();
        }
    return json::object();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scene_material_roundtrip_test\n");

    const fs::path root = fs::temp_directory_path() / "bug0072_material_roundtrip";
    fs::remove_all(root);
    fs::create_directories(root / "assets/models");
    { std::ofstream(root / "assets/models/house.fbx") << "placeholder"; }
    const fs::path scenePath = root / "scenes/test.scene";

    // ── 1. save writes the authored name (the bug) ─────────────────────────
    {
        std::printf("1. save\n");
        FakeHost host; flecs::world w;
        const MeshHandle house = host.addMesh((root / "assets/models/house.fbx").string());
        spawn(w, "house", house, host.brick);
        spawn(w, "plain", house, MaterialHandle{});

        const SceneAssets sa = host.hooks();
        CHECK(SceneSerializer::save(scenePath, w, host.meshes, nullptr, root, &sa), "saved");
        const json s = readJson(scenePath);
        const json h = meshOf(s, "house");
        CHECK(h.value("material", std::string{}) == "Brick",
              "the override is saved by name: %s", h.dump().c_str());
        CHECK(!meshOf(s, "plain").contains("material"), "no override, no key");
        CHECK(!h.contains("matOverrideId"), "never a session handle id on disk");

        // A host with no material hook: nothing to name it by, so no key — and
        // no crash. This is what EVERY save did before the fix.
        const SceneAssets bare = host.hooks(/*withMaterials*/ false);
        CHECK(SceneSerializer::save(scenePath, w, host.meshes, nullptr, root, &bare), "saved bare");
        CHECK(!meshOf(readJson(scenePath), "house").contains("material"), "no hook: no key");
    }

    // ── 2. load applies it, cooked and streamed ────────────────────────────
    {
        std::printf("2. load\n");
        json scene;
        scene["version"] = 1;
        const json xf = {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
        scene["entities"] = json::array({
            {{"name", "cooked"},   {"transform", xf}, {"meshRenderer",
                {{"path", "assets/models/house.fbx"}, {"cookedPath", "meshs/x.cooked"},
                 {"material", "Brick"}}}},
            {{"name", "streamed"}, {"transform", xf}, {"meshRenderer",
                {{"path", "assets/models/house.fbx"}, {"material", "Glass"}}}},
        });
        { std::ofstream(scenePath) << scene.dump(2); }

        FakeHost host; flecs::world w;
        CHECK(SceneSerializer::loadAsync(scenePath, w, host.storage, host.hooks(),
                                         nullptr, root), "loaded");
        flecs::entity c = byName(w, "cooked"), s = byName(w, "streamed");
        const MeshRenderer* cm = c ? c.try_get<MeshRenderer>() : nullptr;
        const MeshRenderer* sm = s ? s.try_get<MeshRenderer>() : nullptr;
        CHECK(cm && cm->materialOverride == host.brick, "cooked mesh: Brick applied");
        CHECK(c && c.has<LodMesh>() && c.get<LodMesh>().count == 1, "cooked mesh: its LOD level wired");
        CHECK(host.streamed == 1, "the source-only mesh went through streamMesh");
        CHECK(sm && sm->materialOverride == host.glass,
              "streamed mesh: Glass applied (the callback used to drop it)");
        CHECK(s && !s.has<UnresolvedMesh>(), "streamed mesh resolved");

        // ── 3. and saves back the same names ──
        std::printf("3. save again\n");
        const SceneAssets sa = host.hooks();
        SceneSerializer::save(scenePath, w, host.meshes, nullptr, root, &sa);
        const json back = readJson(scenePath);
        CHECK(meshOf(back, "cooked").value("material", std::string{}) == "Brick", "Brick round-trips");
        CHECK(meshOf(back, "streamed").value("material", std::string{}) == "Glass", "Glass round-trips");
    }

    // ── 4. a host that cannot stream keeps the reference, says why ─────────
    {
        std::printf("4. no streaming\n");
        json scene;
        scene["version"] = 1;
        scene["entities"] = json::array({{{"name", "streamed"}, {"meshRenderer",
            {{"path", "assets/models/house.fbx"}}}}});
        { std::ofstream(scenePath) << scene.dump(2); }
        FakeHost host; flecs::world w;
        SceneSerializer::loadAsync(scenePath, w, host.storage,
                                   host.hooks(true, /*canStream*/ false), nullptr, root);
        flecs::entity s = byName(w, "streamed");
        const UnresolvedMesh* u = s ? s.try_get<UnresolvedMesh>() : nullptr;
        CHECK(u && !u->pending && u->reason == "no loader",
              "kept, not pending, reason given (%s)", u ? u->reason.c_str() : "-");
    }

    fs::remove_all(root);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
