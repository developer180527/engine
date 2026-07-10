// ── cooker_test — asset cook pipeline gauntlet (cooker audit) ────────────────
// Regressions for the cooker review findings:
//   1. Determinant trap: a bare |det| > 1e-12 singularity check collapsed
//      for small uniform scales (0.0001^3 IS 1e-12) — valid heavily-scaled
//      assets got an IDENTITY normal matrix, so normals stopped following
//      node rotation (broken shading). Repro: rotated + tiny-scaled node,
//      assert the cooked normal actually rotated.
//   2. String table dedup: append-only interning stored one shared mesh
//      path once PER ENTITY (10k-prop scenes bloated by megabytes; the
//      report's O(N^2) claim was false — the real defect was bloat).
//   3. Garbage-in: a corrupt mesh file must FAIL the cook, never crash.
// Headless, no GPU. Exits non-zero on first failure.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "assets/cookers/mesh_cooker.h"
#include "assets/cookers/scene_cooker.h"
#include <assetlib/mesh_asset.h>
#include <assetlib/scene_asset.h>
#include <assetlib/cook_pipeline.h>

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static bool cookMesh(const fs::path& src, const fs::path& out) {
    MeshCooker cooker;
    assetlib::CookContext ctx;
    ctx.sourcePath = src;
    ctx.outputPath = out;
    return cooker.cook(ctx).success;
}

// -90° about X then uniform scale s: the node transform every DCC export
// with a unit-conversion scale produces.
static aiMatrix4x4 rotXNeg90Scaled(float s) {
    aiMatrix4x4 rot, scl;
    aiMatrix4x4::RotationX(-3.14159265358979f / 2.0f, rot);
    aiMatrix4x4::Scaling(aiVector3D(s, s, s), scl);
    return rot * scl;
}

static aiVector3D mul(const aiMatrix3x3& m, const aiVector3D& v) {
    return { m.a1*v.x + m.a2*v.y + m.a3*v.z,
             m.b1*v.x + m.b2*v.y + m.b3*v.z,
             m.c1*v.x + m.c2*v.y + m.c3*v.z };
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cooker_test: asset cook pipeline gauntlet\n");

    const fs::path dir = fs::temp_directory_path() / "engine_cooker_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ── 1. Determinant trap: rotation must survive a 0.0001 scale ────────
    // cookNormalMatrix is the exact function emitMesh bakes normals with.
    // RotationX(-90°) maps +Z into +Y (assimp's convention). Pre-fix,
    // scale 0.0001 tripped the det<=1e-12 guard (0.0001^3 == 1e-12) ->
    // identity normal matrix -> the normal STAYED +Z and shading broke.
    for (float scale : {1.0f, 0.01f, 0.0001f}) {
        const aiMatrix3x3 nm = cookNormalMatrix(rotXNeg90Scaled(scale));
        aiVector3D n = mul(nm, aiVector3D(0, 0, 1));
        n.Normalize();
        CHECK(n.y > 0.9f && std::fabs(n.z) < 0.1f,
              "scale %g: +Z normal rotated to +Y (n=%.3f,%.3f,%.3f) — the trap",
              scale, n.x, n.y, n.z);
    }
    // A GENUINELY singular basis (flattened Z axis) must still fall back.
    {
        aiMatrix4x4 flat;
        aiMatrix4x4::Scaling(aiVector3D(1, 1, 0), flat);
        const aiMatrix3x3 nm = cookNormalMatrix(flat);
        const aiMatrix3x3 identity{};
        CHECK(std::memcmp(&nm, &identity, sizeof nm) == 0,
              "flattened basis still falls back to identity");
    }

    // ── 2. Scene string table dedup ───────────────────────────────────────
    {
        const std::string shared = "assets/props/very/long/shared/mesh_path.gltf";
        std::string scene = R"({"entities":[)";
        for (int i = 0; i < 100; ++i) {
            if (i) scene += ",";
            scene += R"({"id":)" + std::to_string(i + 1)
                   + R"(,"name":"e)" + std::to_string(i)
                   + R"(","meshRenderer":{"path":")" + shared + R"("}})";
        }
        scene += "]}";
        fs::path jsonPath = dir / "dedup.scene";
        { std::ofstream f(jsonPath); f << scene; }
        fs::path outPath = dir / "dedup.cooked";
        CHECK(cookSceneFile(jsonPath, outPath), "scene with 100 shared refs cooks");

        assetlib::SceneAsset cooked;
        CHECK(assetlib::loadScene(cooked, outPath), "cooked scene loads");
        CHECK(cooked.entities.size() == 100, "100 entities (%zu)", cooked.entities.size());
        // Pre-fix: 100 copies of the path (~4.5 KB). Post-fix: one copy +
        // 100 short names. Generous bound proves dedup without brittleness.
        CHECK(cooked.stringTable.size() < shared.size() * 3 + 100 * 8,
              "string table deduplicated (%zu B, was ~%zu pre-fix)",
              cooked.stringTable.size(), shared.size() * 100);
        // Offsets must still round-trip after interning.
        bool allResolve = true;
        for (const auto& e : cooked.entities)
            if (assetlib::stringTableRead(cooked.stringTable,
                    e.meshSourceOffset, e.meshSourceLength) != shared)
                allResolve = false;
        CHECK(allResolve, "every entity's interned path round-trips");
    }

    // ── 3. Garbage in, failure out — never a crash ────────────────────────
    {
        fs::path garbage = dir / "corrupt.fbx";
        { std::ofstream f(garbage, std::ios::binary);
          f << "this is definitely not an fbx file"; }
        fs::path out = dir / "corrupt.cooked";
        CHECK(!cookMesh(garbage, out), "corrupt mesh fails cleanly (no crash)");
    }

    // ── 4. Dependency-check sanity (no registry / missing file) ──────────
    {
        CHECK(!sceneDependsOnNewerAssets(dir / "dedup.scene", {}, nullptr, {}, {}),
              "no registry -> no forced recook");
        CHECK(!sceneDependsOnNewerAssets(dir / "missing.scene", {}, nullptr, {}, {}),
              "missing scene -> no forced recook");
    }

    fs::remove_all(dir);

    if (g_failures) { std::printf("cooker_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("cooker_test: ALL PASS\n");
    return 0;
}
