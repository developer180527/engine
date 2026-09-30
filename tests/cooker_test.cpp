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
#include "assets/cookers/mesh/cook_common.h"
#include "assets/cookers/scene/scene_cooker.h"
#include "assets/cookers/texture/texture_encode.h"
#include "assets/cookers/texture/texture_cooker.h"
#include "assets/importers/gltf_losses.h"
#include "assets/import/frontend_cgltf.h"
#include "animation/cooked_clip.h"
#include <cgltf.h>
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

// The full result, for tests that need the refusal's message.
static assetlib::CookResult cookMeshResult(const fs::path& src, const fs::path& out) {
    MeshCooker cooker;
    assetlib::CookContext ctx;
    ctx.sourcePath = src;
    ctx.outputPath = out;
    return cooker.cook(ctx);
}

// A tiny VALID glTF (it passes cgltf_validate): one triangle, plus the two
// accessors an animation needs, with the skin / animation / mesh switched on
// per case. Written by the test, so no binary fixture is checked in.
// Buffer: pos 0..35, nrm 36..71, idx 72..77, pad, time 80..83, xyz 84..95.
static std::string tinyGltf(bool mesh, bool skin, bool anim) {
    unsigned char buf[168] = {};
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const float nrm[9] = {0,0,1, 0,0,1, 0,0,1};
    const uint16_t idx[3] = {0,1,2};
    const float t = 0.0f, xyz[3] = {0,0,0};
    std::memcpy(buf, pos, 36); std::memcpy(buf + 36, nrm, 36); std::memcpy(buf + 72, idx, 6);
    std::memcpy(buf + 80, &t, 4); std::memcpy(buf + 84, xyz, 12);
    // Skin data for all three vertices: JOINTS_0 = (0,0,0,0), WEIGHTS_0 = (1,0,0,0).
    const float w[4] = {1, 0, 0, 0};
    for (int v = 0; v < 3; ++v) std::memcpy(buf + 120 + v * 16, w, 16);
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;                                   // 168 % 3 == 0: no padding
    for (int i = 0; i < 168; i += 3) {
        const unsigned v = buf[i] << 16 | buf[i+1] << 8 | buf[i+2];
        b64 += tab[(v >> 18) & 63]; b64 += tab[(v >> 12) & 63];
        b64 += tab[(v >> 6) & 63];  b64 += tab[v & 63];
    }
    std::string nodes = mesh ? (skin ? R"([{"mesh":0,"skin":0},{"name":"bone"}])"
                                     : R"([{"mesh":0},{"name":"bone"}])")
                             : R"([{"name":"root"},{"name":"bone"}])";
    std::string j = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],"nodes":)" + nodes;
    if (mesh) j += skin ? R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"JOINTS_0":5,"WEIGHTS_0":6},"indices":2}]}])"
                        : R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}])";
    if (skin) j += R"(,"skins":[{"joints":[1]}])";
    if (anim) j += R"(,"animations":[{"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],)"
                   R"("samplers":[{"input":3,"output":4}]}])";
    j += R"(,"accessors":[
 {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
 {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
 {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"},
 {"bufferView":3,"componentType":5126,"count":1,"type":"SCALAR","min":[0],"max":[0]},
 {"bufferView":4,"componentType":5126,"count":1,"type":"VEC3"},
 {"bufferView":5,"componentType":5123,"count":3,"type":"VEC4"},
 {"bufferView":6,"componentType":5126,"count":3,"type":"VEC4"}],
"bufferViews":[
 {"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},
 {"buffer":0,"byteOffset":72,"byteLength":6},{"buffer":0,"byteOffset":80,"byteLength":4},
 {"buffer":0,"byteOffset":84,"byteLength":12},{"buffer":0,"byteOffset":96,"byteLength":24},
 {"buffer":0,"byteOffset":120,"byteLength":48}],
"buffers":[{"byteLength":168,"uri":"data:application/octet-stream;base64,)" + b64 + R"("}]})";
    return j;
}

// What gltf_losses.h concludes about a file, read the same way the paths read it.
static bool lossesOf(const fs::path& src, GltfLosses& out, bool& valid) {
    cgltf_options o{}; cgltf_data* d = nullptr;
    if (cgltf_parse_file(&o, src.string().c_str(), &d) != cgltf_result_success) return false;
    const bool buffers = cgltf_load_buffers(&o, d, src.string().c_str()) == cgltf_result_success;
    valid = buffers && cgltf_validate(d) == cgltf_result_success;
    out = gltfLosses(*d);
    cgltf_free(d);
    return buffers;
}

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


int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cooker_test: asset cook pipeline gauntlet\n");

    const fs::path dir = fs::temp_directory_path() / "engine_cooker_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // ── 1. Determinant trap: rotation must survive a 0.0001 scale ────────
    // meshcook::normalMatrix is what the back end bakes normals with, for every
    // format (it replaced the Assimp path's cookNormalMatrix, WO-013).
    // RotationX(-90°) maps +Z into +Y. A bare det<=1e-12 guard would trip at
    // scale 0.0001 (0.0001^3 == 1e-12) -> identity -> the normal STAYS +Z.
    auto columnMajor = [](const aiMatrix4x4& m, float out[16]) {
        const float r[4][4] = {{m.a1, m.a2, m.a3, m.a4}, {m.b1, m.b2, m.b3, m.b4},
                               {m.c1, m.c2, m.c3, m.c4}, {m.d1, m.d2, m.d3, m.d4}};
        for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) out[c * 4 + rr] = r[rr][c];
    };
    for (float scale : {1.0f, 0.01f, 0.0001f}) {
        float m[16], nm[9];
        columnMajor(rotXNeg90Scaled(scale), m);
        meshcook::normalMatrix(m, nm);
        float n[3] = {nm[2], nm[5], nm[8]};              // nm * (0,0,1), row-major 3x3
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& c : n) c /= l;
        CHECK(n[1] > 0.9f && std::fabs(n[2]) < 0.1f,
              "scale %g: +Z normal rotated to +Y (n=%.3f,%.3f,%.3f) — the trap", scale, n[0], n[1], n[2]);
    }
    // A GENUINELY singular basis (flattened Z axis) must still fall back.
    {
        aiMatrix4x4 flat;
        aiMatrix4x4::Scaling(aiVector3D(1, 1, 0), flat);
        float m[16], nm[9];
        columnMajor(flat, m);
        meshcook::normalMatrix(m, nm);
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        CHECK(std::memcmp(nm, identity, sizeof nm) == 0, "flattened basis still falls back to identity");
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

    // ── 2c. A skinned glTF cooks WITH its skeleton (WO-014; was WO-002's refusal) ─
    // WO-002 refused these, because the glTF path read meshes only and cooked a
    // skinned .glb as a static mesh. WO-014 reads skins and clips, so the same
    // fixtures now cook with bones: the test inverted, as that order required.
    {
        struct Case { const char* name; bool mesh, skin, anim; const char* expect; uint32_t version, bones; size_t clips; };
        const Case cases[] = {
            {"static",           true,  false, false, "cooks",   2, 0, 0},
            {"skinned",          true,  true,  false, "cooks",   6, 1, 0},
            {"skinned+animated", true,  true,  true,  "cooks",   6, 1, 1},
            {"animation-only",   false, false, true,  "clip",    0, 0, 1},
            {"static+node anim", true,  false, true,  "cooks",   2, 0, 0},
        };
        for (const Case& c : cases) {
            const fs::path src = dir / (std::string("wo002_") + c.name + ".gltf");
            { std::ofstream f(src); f << tinyGltf(c.mesh, c.skin, c.anim); }
            GltfLosses l; bool valid = false;
            CHECK(lossesOf(src, l, valid) && valid, "%s: fixture is a valid glTF", c.name);

            const fs::path out = dir / (std::string("wo002_") + c.name + ".cooked");
            fs::remove(out);
            const assetlib::CookResult r = cookMeshResult(src, out);
            if (std::string(c.expect) == "clip") {
                // WO-016: it used to be skipped, cooked only when the editor first
                // played it. Now it cooks, skeleton-independent, as a clip.
                anim::CookedClip clip; std::string why;
                CHECK(r.success && anim::readCookedClipFile(out, clip, why) && clip.trackBones.size() == 1,
                      "%s: cooks as a clip, keys by bone name (%zu track(s); %s%s)", c.name, clip.trackBones.size(),
                      r.error.c_str(), why.c_str());
                continue;
            }
            assetlib::MeshAsset a;
            const bool loaded = r.success && assetlib::loadMesh(a, out);
            CHECK(loaded && a.header.version == c.version && a.header.boneCount == c.bones && a.clips.size() == c.clips,
                  "%s: cooks as v%u with %u bone(s) and %zu clip(s) (got v%u, %u, %zu; %s)", c.name, c.version, c.bones, c.clips,
                  loaded ? a.header.version : 0u, loaded ? a.header.boneCount : 0u, loaded ? a.clips.size() : (size_t)0, r.error.c_str());
        }
        // Node animation on a model with no skin has nothing to play on: it cooks
        // static, and the loss is reported rather than silent.
        const imp::ImportResult nodeAnim = imp::CgltfFrontend().importScene(dir / "wo002_static+node anim.gltf", {});
        bool reported = false;
        if (nodeAnim) for (const auto& d : nodeAnim.scene().dropped)
            reported |= d.kind == imp::Dropped::Kind::Animation && d.effect == imp::Dropped::Effect::Less;
        CHECK(reported, "static+node anim: the node animation is reported as dropped (Less)");
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

    // ── 2b. Every cooked texture records its COLOUR SPACE (ctex v3) ───────
    // Colour pipeline stage A. Before v3 nothing said whether a texture's bytes
    // were sRGB colour or linear data, and the runtime sampled everything as
    // linear — base colour lit ~2.3x too bright at mid-grey. The encoder always
    // KNEW (it already filtered colour mips in linear light and normal maps as
    // vectors); these pin that it now writes that knowledge down, and that the
    // reader treats old and unknown values the way texture_asset.cpp says.
    {
        std::printf("\n-- 2b. texture colour space --\n");
        std::vector<uint8_t> px(8 * 8 * 4, 200);
        assetlib::TextureAsset colourTex, normalTex;
        CHECK(cook::encodeTexture(px.data(), 8, 8, /*isNormalMap*/ false, colourTex)
              && colourTex.header.version == 3
              && colourTex.header.colourSpace == assetlib::kTexColourSrgb,
              "a colour texture cooks as v3 sRGB (v%u, %s)",
              colourTex.header.version,
              assetlib::texColourSpaceName(colourTex.header.colourSpace));
        CHECK(cook::encodeTexture(px.data(), 8, 8, /*isNormalMap*/ true, normalTex)
              && normalTex.header.colourSpace == assetlib::kTexColourLinear,
              "a normal map cooks as LINEAR — a GPU sRGB decode would bend its "
              "vectors (%s)",
              assetlib::texColourSpaceName(normalTex.header.colourSpace));

        const fs::path cs = dir / "colour.ctex";
        assetlib::TextureAsset csBack;
        CHECK(assetlib::saveTexture(colourTex, cs) && assetlib::loadTexture(csBack, cs)
              && csBack.header.colourSpace == assetlib::kTexColourSrgb,
              "the colour space survives save -> load");

        // A v2 file: version 2, and junk in what are now the colour-space and
        // pad bytes. The reader must call it LEGACY by version rather than
        // believing a byte that no v2 writer promised to mean anything.
        assetlib::TextureAsset v2 = colourTex;
        v2.header.version = 2;
        v2.header.colourSpace = 0xAB;
        const fs::path v2p = dir / "v2_junkpad.ctex";
        assetlib::TextureAsset v2b;
        CHECK(assetlib::saveTexture(v2, v2p) && assetlib::loadTexture(v2b, v2p)
              && v2b.header.colourSpace == assetlib::kTexColourLegacy,
              "a v2 texture reads as legacy whatever its pad bytes held (%u)",
              (unsigned)v2b.header.colourSpace);

        // A v3 file naming a colour space this build does not know is REFUSED,
        // like an unknown format id — guessing renders it wrong with no log line.
        assetlib::TextureAsset future = colourTex;
        future.header.colourSpace = assetlib::kTexColourCount;
        const fs::path fp = dir / "future_colour.ctex";
        assetlib::TextureAsset fb;
        CHECK(assetlib::saveTexture(future, fp) && !assetlib::loadTexture(fb, fp),
              "a v3 texture with an unknown colour space is refused");

        // Both cookers that write .ctex must re-cook on the format bump, or a
        // cache hit hands back a v2 blob and the runtime falls to legacy.
        assetlib::CookContext ctx;
        ctx.sourcePath = dir / "rock_diff.png";
        CHECK(TextureCooker{}.settingsFingerprint(ctx).find(";ctex=3") != std::string::npos,
              "the texture cooker's fingerprint names ctex v3 (%s)",
              TextureCooker{}.settingsFingerprint(ctx).c_str());
        CHECK(MeshCooker{}.settingsFingerprint(ctx).find(";ctex=3") != std::string::npos,
              "and so does the mesh cooker's, which writes sibling .ctex files (%s)",
              MeshCooker{}.settingsFingerprint(ctx).c_str());
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
