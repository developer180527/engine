// ── material_name_test — materials addressed by authored NAME ───────────────
//
// loadMaterialAsset takes a name ("zombie_sickly"), not a cooked <uuid>.cooked
// path, and memoizes one Material per name because a spawner calls it per
// entity. That memo is the whole point and also the whole risk: a MaterialHandle
// is a bare slot index over a free list with NO generation counter, so a stale
// cache entry does not go invalid — it silently starts naming whatever material
// later took the slot. This gauntlet pins the three ways that goes wrong:
//
//   1. unload must evict the name entry, or "rust" comes back as the material
//      that reused its slot — a plausible-looking wrong render, which is exactly
//      what loadMaterialAsset's contract says must never happen.
//   2. the index must not latch as "built" before there is a cache root to
//      scan: m_cacheRoot is late-bound at project open, and the editor boots
//      projectless, so an early materialNames() would otherwise poison the
//      index empty for the process lifetime.
//   3. a missing name must stay a stable, cheap failure however many times it
//      is asked for.
//
// Headless on bgfx Noop, and the fixtures carry no textures, so nothing here
// needs a GPU.
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <bgfx/bgfx.h>

#include <assetlib/material_asset.h>

#include "runtime/services/asset_service.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// No textures: a material that names none never reaches resolveTexture, which
// is what keeps this test GPU-free.
static bool writeMaterial(const fs::path& p, const std::string& name,
                          const std::string& shader) {
    assetlib::MaterialAsset m;
    m.name       = name;
    m.shaderName = shader;
    m.uniforms.push_back({ "u_params", { 0.0f, 0.5f, 0.0f, 0.0f } });
    return assetlib::saveMaterial(m, p);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("material_name_test: materials by authored name\n");

    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 16; init.resolution.height = 16;
    if (!bgfx::init(init)) { std::printf("FAIL bgfx\n"); return 1; }

    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "engine_material_name_test";
    const fs::path dir  = root / ".cache" / "materials";
    fs::remove_all(root, ec);
    fs::create_directories(dir, ec);
    // Deliberately named so the SORT order (a-, b-) is the reverse of the order
    // the test loads them in — a resolution that depended on scan order would
    // show up here rather than on someone else's machine.
    if (!writeMaterial(dir / "b-uuid.cooked", "rust", "standard")
     || !writeMaterial(dir / "a-uuid.cooked", "zombie_sickly", "skinned")) {
        std::printf("FAIL fixture\n");
        return 1;
    }

    {
        AssetRegistry    meshes;
        TextureRegistry  textures;
        MaterialRegistry materials;
        AssetService svc({meshes, textures, materials});

        // ═══ 1. the index must survive being asked too early ════════════════
        // No project root yet — this is the editor's projectless boot.
        CHECK(svc.materialNames().empty(),
              "with no project open, no material names");
        CHECK(!svc.loadMaterialAsset("rust").valid(),
              "...and nothing loads by name");

        svc.setProjectRoot(root);

        const auto names = svc.materialNames();
        CHECK(names.size() == 2, "after project open, both names index (%zu)",
              names.size());
        CHECK(names.size() == 2 && names[0] == "rust"
                                && names[1] == "zombie_sickly",
              "names come back sorted");

        // ═══ 2. one Material per name ═══════════════════════════════════════
        const MaterialHandle rust = svc.loadMaterialAsset("rust");
        CHECK(rust.valid(), "\"rust\" loads");
        CHECK(svc.loadMaterialAsset("rust").id == rust.id,
              "a second ask returns the SAME handle, not a second slot");
        CHECK(svc.materialCount() == 1, "...and only one material exists (%zu)",
              svc.materialCount());
        const Material* m = materials.getMaterial(rust);
        CHECK(m && m->shaderName == "standard",
              "\"rust\" resolved to its own shader");

        // ═══ 3. unload evicts the name, and a reused slot cannot alias ══════
        CHECK(svc.unloadMaterial(rust), "\"rust\" unloads");

        // This is the trap: the free list hands the SAME slot back out, so a
        // surviving cache entry for "rust" would now name the zombie material.
        const MaterialHandle zombie = svc.loadMaterialAsset("zombie_sickly");
        CHECK(zombie.valid(), "\"zombie_sickly\" loads");
        CHECK(zombie.id == rust.id,
              "...into the freed slot (id %u) — the aliasing setup is real",
              zombie.id);

        const MaterialHandle rust2 = svc.loadMaterialAsset("rust");
        CHECK(rust2.valid(), "\"rust\" loads again after being unloaded");
        CHECK(rust2.id != zombie.id,
              "...as a DISTINCT handle, not the recycled zombie slot");
        const Material* m2 = materials.getMaterial(rust2);
        CHECK(m2 && m2->shaderName == "standard",
              "...and it is rust, not zombie wearing rust's name");
        const Material* mz = materials.getMaterial(zombie);
        CHECK(mz && mz->shaderName == "skinned",
              "zombie_sickly is undisturbed by rust's reload");

        // ═══ 4. a missing name is a stable, cheap failure ═══════════════════
        for (int i = 0; i < 3; ++i)
            CHECK(!svc.loadMaterialAsset("no_such_material").valid(),
                  "an unknown name fails (ask %d)", i + 1);
        CHECK(svc.materialCount() == 2,
              "a failed lookup allocated nothing (%zu materials)",
              svc.materialCount());
        CHECK(!svc.loadMaterialAsset("").valid(), "the empty name fails");
        CHECK(!svc.loadMaterialAsset(nullptr).valid(), "a null name fails");
    }

    fs::remove_all(root, ec);
    bgfx::shutdown();

    if (g_failures) {
        std::printf("material_name_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("material_name_test: all checks passed\n");
    return 0;
}
