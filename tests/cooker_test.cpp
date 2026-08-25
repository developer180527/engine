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

#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/scene/scene_cooker.h"
#include "assets/cookers/texture/texture_encode.h"
// Assimp's matrix members are inline templates defined in .inl headers this
// TU must instantiate ITSELF: with assimp built -O0 the archive happened to
// carry weak out-of-line copies to link against, but an optimized assimp
// inlines them and exports nothing.
#include <assimp/vector3.inl>
#include <assimp/matrix3x3.inl>
#include <assimp/matrix4x4.inl>
#include <assetlib/mesh_asset.h>
#include <assetlib/scene_asset.h>
#include <assetlib/texture_asset.h>
#include <assetlib/cook_pipeline.h>
#include <vector>
#include <cmath>
#include "test_env.h"

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

    // ── 2b. glTF cook (cgltf path): transforms bake, normals survive ─────
    // One +Z triangle under a node rotated -90° about X and scaled 0.0001 —
    // the determinant-trap scenario through the REAL glTF pipeline.
    {
        unsigned char buf[80] = {};
        float pos[9] = {0,0,0, 1,0,0, 0,1,0};
        float nrm[9] = {0,0,1, 0,0,1, 0,0,1};
        uint16_t idx[3] = {0,1,2};
        std::memcpy(buf,      pos, 36);
        std::memcpy(buf + 36, nrm, 36);
        std::memcpy(buf + 72, idx, 6);
        static const char* tab =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        // 80 % 3 == 2, so the final group is SHORT. Reading buf[i+1]/buf[i+2]
        // unconditionally walked one byte past the array — caught by ASan and
        // UBSan the first time those lanes ran (BUG-0002).
        //
        // Zero-filling the missing byte is also the correct encoding: the third
        // base64 character of a 2-byte group takes its low 2 bits from the byte
        // that is not there, so the old code emitted a character derived from
        // stack garbage. It round-tripped only because the decoder discards
        // that byte as padding.
        for (int i = 0; i < 80; i += 3) {
            const unsigned b1 = (i + 1 < 80) ? buf[i+1] : 0u;
            const unsigned b2 = (i + 2 < 80) ? buf[i+2] : 0u;
            unsigned v = buf[i] << 16 | b1 << 8 | b2;
            b64 += tab[(v >> 18) & 63]; b64 += tab[(v >> 12) & 63];
            b64 += tab[(v >> 6) & 63];  b64 += tab[v & 63];
        }
        b64[b64.size()-1] = '=';   // 80 % 3 == 2 -> one pad char

        char json[2048];
        // -90 deg about X: quaternion (x,y,z,w) = (-sin45, 0, 0, cos45)
        std::snprintf(json, sizeof json, R"({
"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"mesh":0,"rotation":[-0.7071068,0,0,0.7071068],"scale":[0.0001,0.0001,0.0001]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],
"accessors":[
 {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
 {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
 {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
"bufferViews":[
 {"buffer":0,"byteOffset":0,"byteLength":36},
 {"buffer":0,"byteOffset":36,"byteLength":36},
 {"buffer":0,"byteOffset":72,"byteLength":6}],
"buffers":[{"byteLength":80,"uri":"data:application/octet-stream;base64,%s"}]})",
            b64.c_str());
        fs::path src = dir / "tiny.gltf";
        { std::ofstream f(src); f << json; }
        fs::path out = dir / "tiny_gltf.cooked";
        CHECK(cookMesh(src, out), "glTF cooks via cgltf path");

        assetlib::MeshAsset asset;
        CHECK(assetlib::loadMesh(asset, out), "cooked glTF loads");
        CHECK(asset.header.vertexCount == 3 && asset.header.indexCount == 3,
              "glTF geometry counts (v=%u i=%u)",
              asset.header.vertexCount, asset.header.indexCount);
        if (asset.vertexData.size() >= 48) {
            struct V { float px,py,pz,nx,ny,nz,tx,ty,tz,tw,u,v; } v{};
            std::memcpy(&v, asset.vertexData.data(), sizeof v);
            // RotX(-90) maps +Z -> -Y in glTF's column-vector convention
            // (q=(-sin45,0,0,cos45)); magnitude survives the 0.0001 scale.
            CHECK(std::fabs(std::fabs(v.ny) - 1.0f) < 0.1f
                  && std::fabs(v.nz) < 0.1f,
                  "glTF normal rotated at scale 0.0001 (n=%.3f,%.3f,%.3f)",
                  v.nx, v.ny, v.nz);
        }
    }

    // ── 2c. BC texture encode: format, mips, packed size, v1 compat ──────
    {
        // 8x8 OPAQUE gradient (alpha=255). Default (fast) path → BC1, a 4-level
        // mip chain (8,4,2,1). BC1 is 8 B/block: 8x8=4 blocks, then 1+1+1 →
        // 7 blocks × 8 B = 56 B. This is the iteration default — BC7's bimg
        // encoder is exhaustive (minutes per 4K), so it's opt-in only.
        std::vector<uint8_t> rgba(8 * 8 * 4);
        for (int i = 0; i < 8 * 8; ++i) {
            rgba[i*4+0] = (uint8_t)(i * 4);
            rgba[i*4+1] = (uint8_t)(255 - i * 4);
            rgba[i*4+2] = 128; rgba[i*4+3] = 255;   // opaque
        }
        testenv::unset("COOK_TEX_HQ");
        assetlib::TextureAsset t;
        CHECK(cook::encodeTexture(rgba.data(), 8, 8, false, t),
              "opaque color encodes (fast default)");
        CHECK(t.header.format == assetlib::kTexBC1 && t.header.mipCount == 4,
              "opaque → BC1 + 4 mips (fmt=%u mips=%u)", t.header.format, t.header.mipCount);
        CHECK(t.pixels.size() == 56,
              "BC1 packed mip chain is 56 B (%zu)", t.pixels.size());

        // Same pixels but with real alpha → BC3 (4:1, still fast squish).
        std::vector<uint8_t> rgbaA = rgba;
        rgbaA[3] = 100;                              // one non-opaque texel
        assetlib::TextureAsset ta;
        CHECK(cook::encodeTexture(rgbaA.data(), 8, 8, false, ta)
              && ta.header.format == assetlib::kTexBC3 && ta.pixels.size() == 112,
              "alpha color → BC3 (fmt=%u, %zu B)", ta.header.format, ta.pixels.size());

        // Opt-in HQ → BC7 (16 B/block → 112 B), the exhaustive final-bake path.
        testenv::set("COOK_TEX_HQ", "1");
        assetlib::TextureAsset hq;
        CHECK(cook::encodeTexture(rgba.data(), 8, 8, false, hq)
              && hq.header.format == assetlib::kTexBC7 && hq.pixels.size() == 112,
              "COOK_TEX_HQ → BC7 (fmt=%u, %zu B)", hq.header.format, hq.pixels.size());
        testenv::unset("COOK_TEX_HQ");

        assetlib::TextureAsset n;
        CHECK(cook::encodeTexture(rgba.data(), 8, 8, true, n)
              && n.header.format == assetlib::kTexBC5,
              "normal maps encode BC5");
        CHECK(cook::looksLikeNormalMap("service_pistol_nor_gl_4k.jpg")
              && !cook::looksLikeNormalMap("service_pistol_diff_4k.jpg"),
              "normal-map filename heuristic");

        // v2 round-trip + v1 back-compat through the same loader.
        fs::path v2 = dir / "enc.ctex";
        CHECK(assetlib::saveTexture(t, v2), "v2 texture saves");
        assetlib::TextureAsset back;
        CHECK(assetlib::loadTexture(back, v2)
              && back.header.format == assetlib::kTexBC1
              && back.header.mipCount == 4
              && back.pixels.size() == 56,
              "v2 texture round-trips (blocks + mips intact)");

        assetlib::TextureAsset v1;
        v1.header.version = 1; v1.header.format = 0; v1.header.mipCount = 0;
        v1.header.width = 2; v1.header.height = 2;
        v1.pixels.assign(16, 0x7F);
        fs::path v1p = dir / "legacy.ctex";
        CHECK(assetlib::saveTexture(v1, v1p), "v1 texture saves");
        assetlib::TextureAsset v1b;
        CHECK(assetlib::loadTexture(v1b, v1p)
              && v1b.header.format == assetlib::kTexRGBA8
              && v1b.header.mipCount == 1
              && v1b.pixels.size() == 16,
              "v1 legacy texture still loads (format 0, 1 mip)");
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

    // ── LOD levels keep the parent's material groups ─────────────────────────
    // End to end: MeshCooker -> decimate -> saveMesh -> loadMesh. decimate_test
    // proves the decimator preserves ranges and fuzz_mesh_loader_test proves the
    // FORMAT round-trips them; the plumbing between the two is a copy loop in
    // appendLodLevels, and this is what catches it being dropped or mis-mapped.
    //
    // Without it, a level draws as one range with material[0] — so a prop with two
    // material groups CHANGED COLOUR the instant it crossed an LOD threshold, and
    // 96 of the MegaKit's 176 meshes have more than one group.
    {
        // A dense two-material mesh: dense enough to clear the cooker's 2 000
        // triangle floor for LOD, and split across two materials.
        const fs::path obj = dir / "lodmat.obj";
        const fs::path mtl = dir / "lodmat.mtl";
        {
            std::ofstream m(mtl);
            m << "newmtl matA\nKd 1 0 0\nnewmtl matB\nKd 0 1 0\n";
        }
        {
            std::ofstream f(obj);
            f << "mtllib lodmat.mtl\n";
            constexpr int kN = 60;                    // 60x60 quads = 7 200 tris
            for (int z = 0; z <= kN; ++z)
                for (int x = 0; x <= kN; ++x)
                    f << "v " << x << " 0 " << z << "\n";
            auto at = [&](int x, int z) { return z * (kN + 1) + x + 1; };  // OBJ is 1-based
            for (int half = 0; half < 2; ++half) {
                f << "usemtl " << (half ? "matB" : "matA") << "\n";
                for (int z = half * kN / 2; z < (half + 1) * kN / 2; ++z)
                    for (int x = 0; x < kN; ++x) {
                        f << "f " << at(x, z) << " " << at(x + 1, z) << " " << at(x, z + 1) << "\n";
                        f << "f " << at(x + 1, z) << " " << at(x + 1, z + 1) << " " << at(x, z + 1) << "\n";
                    }
            }
        }

        const fs::path out = dir / "lodmat.cooked";
        CHECK(cookMesh(obj, out), "a dense two-material mesh cooks");

        assetlib::MeshAsset ma;
        CHECK(assetlib::loadMesh(ma, out), "and loads back");
        CHECK(ma.header.submeshCount >= 2,
              "the parent has %u material group(s)", ma.header.submeshCount);
        CHECK(!ma.lods.empty(), "the cooker emitted LOD levels (%zu)", ma.lods.size());
        CHECK(ma.header.version >= 5,
              "written as v%u — the version that encodes level ranges",
              ma.header.version);

        if (!ma.lods.empty() && ma.header.submeshCount >= 2) {
            bool everyLevelKeepsGroups = true, everyLevelCheaper = true,
                 rangesTile = true, materialsPreserved = true;
            uint32_t parentTris = ma.header.indexCount / 3;
            for (const auto& l : ma.lods) {
                if (l.submeshes.size() < 2) everyLevelKeepsGroups = false;
                const uint32_t tris = l.indexCount / 3;
                if (tris == 0 || tris >= parentTris) everyLevelCheaper = false;
                parentTris = tris;

                uint32_t walk = 0;
                for (const auto& s : l.submeshes) {
                    if (s.indexOffset != walk) rangesTile = false;
                    walk += s.indexCount;
                    // The material index must address the mesh's embedded
                    // material list, or AssetService falls back to material[0]
                    // and the whole fix is undone one layer down.
                    if (s.materialIndex >= ma.materials.size()) materialsPreserved = false;
                }
                if (walk != l.indexCount) rangesTile = false;
            }
            CHECK(everyLevelKeepsGroups,
                  "every level keeps BOTH material groups, not just material[0]");
            CHECK(everyLevelCheaper,
                  "and every level is strictly cheaper than the one above it");
            CHECK(rangesTile,
                  "level ranges tile the level's index buffer — what "
                  "Mesh::submeshesTile() needs for the shadow pass's one-draw path");
            CHECK(materialsPreserved,
                  "and every range's materialIndex addresses a real material");
        }
    }

    fs::remove_all(dir);

    if (g_failures) { std::printf("cooker_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("cooker_test: ALL PASS\n");
    return 0;
}
