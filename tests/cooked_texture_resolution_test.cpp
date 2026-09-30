// ── cooked_texture_resolution_test — does a cooked mesh find its texture? ─────
//
// WO-013, written FIRST, before the Assimp formats moved to the import pipeline:
// the suspicion (imported-scene.md §7.1) was that a cooked static OBJ/FBX whose
// material names an EXTERNAL texture loads untextured, because the cooker stored
// only the texture's basename and the runtime resolves it against the COOKED
// file's directory. This is the fixture that decides it, through the real path:
// MeshCooker, then AssetService::loadMesh on bgfx Noop, then the material's
// base-colour binding.
//
// It asserts the behaviour we WANT (textured), so it was red on the old cook
// path and is green on the new one; the result on the old path is recorded in
// docs/work/WO-013.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>
#include "gpu_test_device.h"

#include "assets/cookers/mesh/mesh_cooker.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "runtime/services/asset_service.h"

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

static void writeTga(const fs::path& p) {                 // 4x4, 32 bpp, uncompressed
    std::vector<uint8_t> t(18, 0);
    t[2] = 2; t[12] = 4; t[14] = 4; t[16] = 32; t[17] = 8;
    for (int i = 0; i < 16; ++i) { t.push_back(40); t.push_back(80); t.push_back(200); t.push_back(255); }
    std::ofstream(p, std::ios::binary).write((const char*)t.data(), (std::streamsize)t.size());
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cooked_texture_resolution_test\n");
    if (!initTestDevice()) return 1;

    const fs::path root  = fs::temp_directory_path() / "wo013_texture_resolution";
    const fs::path cache = root / ".cache" / "meshs";
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    fs::create_directories(cache);

    // A textured OBJ, laid out the way a project has it: model, material library
    // and texture side by side in assets/.
    { std::ofstream(root / "assets" / "crate.mtl") << "newmtl Crate\nKd 1 1 1\nmap_Kd crate.tga\n"; }
    { std::ofstream(root / "assets" / "crate.obj") << "mtllib crate.mtl\nusemtl Crate\n"
                                                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n"
                                                      "f 1/1 2/2 3/3\n"; }
    writeTga(root / "assets" / "crate.tga");

    assetlib::CookContext ctx;
    ctx.sourcePath = root / "assets" / "crate.obj";
    ctx.outputPath = cache / "crate.cooked";
    const auto r = MeshCooker{}.cook(ctx);
    CHECK(r.success, "the OBJ cooks (%s)", r.error.c_str());

    assetlib::MeshAsset cooked;
    const bool read = assetlib::loadMesh(cooked, ctx.outputPath);
    std::string stored;                                    // the first material that names a texture
    if (read) for (const auto& m : cooked.materials) if (m.baseColorPath[0]) { stored = m.baseColorPath; break; }
    std::printf("        the cooked material names its base colour \"%s\"; the runtime looks for it at\n"
                "        %s\n", stored.c_str(), (cache / stored).string().c_str());

    {
        AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
        AssetService svc({meshes, textures, materials});
        svc.setProjectRoot(root);
        const MeshHandle h = svc.loadMesh("meshs/crate.cooked");
        const Mesh* mesh = h.valid() ? meshes.getMesh(h) : nullptr;
        CHECK(mesh != nullptr, "the cooked mesh loads through AssetService");

        bool textured = false;
        if (mesh)
            if (const Material* m = materials.getMaterial(mesh->material))
                for (const auto& b : m->textureBinds)
                    textured |= b.uniform == "s_baseColor" && b.texture.valid();
        CHECK(textured, "and its material's base colour is bound to the texture, not the white fallback");
    }

    shutdownTestDevice();
    fs::remove_all(root);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
