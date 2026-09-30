// ── mesh_backend_test — ImportedScene -> cooked mesh (WO-011) ──────────────────
//
// The one cook back end, tested through ImportedScene values only. No file is
// parsed, as WO-011 requires: the back end is built against the contract, not
// against a parser. Every case cooks to a temp directory and reads the result
// back with assetlib::loadMesh, so what is checked is the cooked BYTES the
// runtime will load, not an in-memory intermediate.
//
// Scenes come from the contract suite's references (import_contract.h) where
// one fits, so the back end and every front end are judged against the same
// meaning.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <assetlib/mesh_asset.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include "assets/cookers/mesh/mesh_backend.h"
#include "import_contract.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace imp;
using impcontract::Case;

static fs::path g_dir;

struct Cooked {
    assetlib::CookResult result;
    meshcook::BackendReport report;
    assetlib::MeshAsset asset;
    bool loaded = false;
    fs::path path;
};
static Cooked cook(const ImportedScene& s, const std::string& name) {
    Cooked c;
    assetlib::CookContext ctx;
    ctx.sourcePath = g_dir / "src" / (name + ".fake");
    ctx.outputPath = g_dir / "out" / (name + ".cooked");
    fs::remove(ctx.outputPath);
    c.path = ctx.outputPath;
    c.result = meshcook::cookImportedScene(s, ctx, &c.report);
    if (c.result.success) c.loaded = assetlib::loadMesh(c.asset, ctx.outputPath);
    return c;
}

struct V48 { float px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v; };
struct V68 { float px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v; uint8_t j[4]; float w[4]; };
static V48 vtx(const assetlib::MeshAsset& a, uint32_t i) { V48 v; std::memcpy(&v, a.vertexData.data() + i * 48, 48); return v; }
static V68 skv(const assetlib::MeshAsset& a, uint32_t i) { V68 v; std::memcpy(&v, a.vertexData.data() + i * 68, 68); return v; }
static uint32_t idx(const assetlib::MeshAsset& a, uint32_t i) {
    if (a.header.indexStride == 2) { uint16_t x; std::memcpy(&x, a.indexData.data() + i * 2, 2); return x; }
    uint32_t x; std::memcpy(&x, a.indexData.data() + i * 4, 4); return x;
}
static bool near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }
static bool near3(float x, float y, float z, float ex, float ey, float ez) { return near(x, ex) && near(y, ey) && near(z, ez); }
static bool hasNote(const Cooked& c, const std::string& needle) {
    for (const auto& n : c.report.notes) if (n.find(needle) != std::string::npos) return true;
    return false;
}

// The triangle's winding agrees with its vertex normal: the front face faces
// where the normal points. This is what a mirrored instance breaks.
static bool windingMatchesNormal(const assetlib::MeshAsset& a, uint32_t tri) {
    const V48 p = vtx(a, idx(a, tri * 3)), q = vtx(a, idx(a, tri * 3 + 1)), r = vtx(a, idx(a, tri * 3 + 2));
    const float e1[3] = {q.px - p.px, q.py - p.py, q.pz - p.pz}, e2[3] = {r.px - p.px, r.py - p.py, r.pz - p.pz};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    return n[0] * p.nx + n[1] * p.ny + n[2] * p.nz > 0;
}

// A 4x4 uncompressed TGA (one full BC block) — stb_image here is built with PNG,
// JPEG and TGA only.
static std::vector<uint8_t> tga(uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> t(18, 0);
    t[2] = 2; t[12] = 4; t[14] = 4; t[16] = 32; t[17] = 8;   // truecolour, 4x4, 32 bpp
    for (int i = 0; i < 16; ++i) { t.push_back(b); t.push_back(g); t.push_back(r); t.push_back(255); }
    return t;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("mesh_backend_test\n");
    g_dir = fs::temp_directory_path() / "wo011_mesh_backend";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir / "src");
    fs::create_directories(g_dir / "out");

    // ── 1. Refusals: nothing written, and a reason that names the problem ────
    std::printf("1. refusals\n");
    {
        ImportedScene bad = impcontract::expected(Case::UnitTriangle);
        bad.meshes[0].indices[2] = 9;
        Cooked c = cook(bad, "invalid");
        CHECK(!c.result.success && c.result.error.find("indices") != std::string::npos && !fs::exists(c.path),
              "an invalid scene is refused, naming the check: %s", c.result.error.c_str());

        ImportedScene wrong = impcontract::expected(Case::UnitTriangle);
        wrong.dropped = {{Dropped::Kind::Skin, Dropped::Effect::Wrong, 1, "1 skin on 'Body'"}};
        c = cook(wrong, "wrong");
        CHECK(!c.result.success && c.result.error.find("skin: 1 skin on 'Body'") != std::string::npos && !fs::exists(c.path),
              "a Wrong loss is refused, naming it: %s", c.result.error.c_str());

        ImportedScene clipsOnly = impcontract::expected(Case::SkinnedColumn);
        clipsOnly.meshes.clear(); clipsOnly.nodes[0].meshes.clear(); clipsOnly.materials.clear();
        c = cook(clipsOnly, "clips_only");
        CHECK(!c.result.success && c.result.skipped && !fs::exists(c.path),
              "a scene with clips and no triangles is skipped for the clip cooker: %s", c.result.error.c_str());

        ImportedScene many = impcontract::expected(Case::SkinnedColumn);
        for (int i = 2; i < 300; ++i) {
            Bone b; b.name = "B" + std::to_string(i); b.parent = 0;
            many.skeleton->bones.push_back(b);
        }
        c = cook(many, "too_many_bones");
        CHECK(!c.result.success && c.result.error.find("300 bones") != std::string::npos,
              "more bones than a cooked vertex can index is refused: %s", c.result.error.c_str());
    }

    // ── 2. A static mesh, field by field ────────────────────────────────────
    std::printf("2. static: the unit triangle\n");
    {
        Cooked c = cook(impcontract::expected(Case::UnitTriangle), "tri");
        const auto& a = c.asset;
        CHECK(c.result.success && c.loaded, "cooks and loads back (%s)", c.result.error.c_str());
        CHECK(a.header.version == 2 && a.header.vertexStride == 48 && a.header.boneCount == 0
              && a.header.vertexCount == 3 && a.header.indexCount == 3 && a.header.indexStride == 2,
              "static layout: v2, 48-byte vertices, 16-bit indices");
        CHECK(idx(a, 0) == 0 && idx(a, 1) == 1 && idx(a, 2) == 2, "winding kept");
        const V48 v1 = vtx(a, 1);
        CHECK(near3(v1.px, v1.py, v1.pz, 1, 0, 0) && near3(v1.nx, v1.ny, v1.nz, 0, 0, 1) && near(v1.u, 1) && near(v1.v, 1),
              "position, normal and UV of vertex 1");
        // No tangents in the source: generated. u grows along +X; v grows along
        // -Y (top-left UV origin), so cross(N, T) = +Y points AGAINST dP/dv: w = -1.
        CHECK(near3(v1.tx, v1.ty, v1.tz, 1, 0, 0) && v1.tw == -1.0f,
              "a missing tangent is generated: (%g %g %g, w %g), want (1 0 0, w -1)", v1.tx, v1.ty, v1.tz, v1.tw);
        CHECK(near3(a.header.boundsMin[0], a.header.boundsMin[1], a.header.boundsMin[2], 0, 0, 0) &&
              near3(a.header.boundsMax[0], a.header.boundsMax[1], a.header.boundsMax[2], 1, 1, 0), "bounds");
        CHECK(a.materials.size() == 1 && near(a.materials[0].roughness, 0.7f) && a.materials[0].flags == 0,
              "one material, default roughness, no textures");
        CHECK(windingMatchesNormal(a, 0), "the front face faces along the normal");

        ImportedScene withT = impcontract::expected(Case::UnitTriangle);
        withT.meshes[0].tangents.assign(3, {0, 1, 0, 1});
        const V48 t = vtx(cook(withT, "tri_tangents").asset, 0);
        CHECK(near3(t.tx, t.ty, t.tz, 0, 1, 0) && t.tw == 1.0f, "a source tangent is kept, handedness included");
    }

    // ── 3. Node transforms are baked; a mirrored instance is not inside-out ─
    std::printf("3. baking\n");
    {
        Cooked c = cook(impcontract::expected(Case::NodeTransforms), "nodes");
        const auto& a = c.asset;
        CHECK(c.loaded && a.header.vertexCount == 6 && a.header.submeshCount == 2,
              "one mesh on two nodes cooks as two instances");
        const V48 r = vtx(a, 1);                          // Raised: (1,0,0) + (0,2,0)
        CHECK(near3(r.px, r.py, r.pz, 1, 2, 0), "the first instance is translated");
        const V48 t = vtx(a, 4);                          // Turned: rotY90 maps +X -> -Z, then raised
        CHECK(near3(t.px, t.py, t.pz, 0, 2, -1) && near3(t.nx, t.ny, t.nz, 1, 0, 0) && near3(t.tx, t.ty, t.tz, 0, 0, -1),
              "the second is rotated: position, normal (+Z -> +X) and tangent (+X -> -Z)");
        CHECK(idx(a, 3) == 3 && windingMatchesNormal(a, 0) && windingMatchesNormal(a, 1),
              "indices rebased per instance; both face the right way");

        ImportedScene mirror = impcontract::expected(Case::UnitTriangle);
        mirror.nodes[0].local.m[0] = -1;                  // scale x by -1: a mirrored instance
        Cooked m = cook(mirror, "mirror");
        const V48 mv = vtx(m.asset, 1);
        CHECK(near3(mv.px, mv.py, mv.pz, -1, 0, 0), "a mirrored instance's positions are mirrored");
        CHECK(idx(m.asset, 1) == 2 && idx(m.asset, 2) == 1, "its winding is flipped back ...");
        CHECK(windingMatchesNormal(m.asset, 0), "... so its front face still faces along its normal (not inside-out)");
        CHECK(mv.tw == 1.0f && near3(mv.tx, mv.ty, mv.tz, -1, 0, 0), "and its tangent handedness flips with it");
    }

    // ── 4. Submeshes and materials ──────────────────────────────────────────
    std::printf("4. submeshes and materials\n");
    {
        Cooked c = cook(impcontract::expected(Case::TwoMaterialQuad), "quad");
        const auto& a = c.asset;
        CHECK(a.submeshes.size() == 2 && a.submeshes[0].indexOffset == 0 && a.submeshes[0].indexCount == 3 &&
              a.submeshes[1].indexOffset == 3 && a.submeshes[1].materialIndex == 1, "two submeshes, in order, own materials");
        CHECK(a.materials.size() == 2 && near(a.materials[0].baseColorFactor[0], 1) && near(a.materials[1].baseColorFactor[2], 1),
              "two materials, red then blue");

        ImportedScene big = impcontract::expected(Case::UnitTriangle);
        Mesh& m = big.meshes[0];
        m.positions.resize(66000, {0, 0, 0}); m.normals.resize(66000, {0, 0, 1}); m.uv0.resize(66000, {0, 0});
        m.indices = {0, 1, 2, 65999, 1, 2};
        m.submeshes = {{0, 6, 0}};
        m.positions[65999] = {0, 0, 1};
        Cooked b = cook(big, "big");
        CHECK(b.loaded && b.asset.header.indexStride == 4 && idx(b.asset, 3) == 65999,
              "more than 65535 vertices switches to 32-bit indices");
    }

    // ── 5. Textures: one rule, every texture a deduplicated sibling ─────────
    std::printf("5. textures\n");
    {
        ImportedScene s = impcontract::expected(Case::TwoMaterialQuad);
        s.materials[0].baseColor = {"", tga(255, 0, 0), "red"};
        s.materials[1].baseColor = {"", tga(255, 0, 0), "red again"};      // identical bytes
        { std::ofstream f(g_dir / "src" / "blue.tga", std::ios::binary); const auto b = tga(0, 0, 255); f.write((const char*)b.data(), (std::streamsize)b.size()); }
        s.materials[1].normal = {"blue.tga", {}, ""};                        // external, relative to the source
        s.materials[0].normal = {"missing.tga", {}, ""};
        Cooked c = cook(s, "textured");
        const auto& a = c.asset;
        CHECK(c.loaded && (a.materials[0].flags & assetlib::kMatFlag_HasBaseColor) &&
              std::string(a.materials[0].baseColorPath) == std::string(a.materials[1].baseColorPath),
              "two materials with identical embedded bytes share one sibling (%s)", a.materials[0].baseColorPath);
        CHECK(fs::exists(g_dir / "out" / a.materials[0].baseColorPath), "the embedded texture is written beside the mesh");
        CHECK((a.materials[1].flags & assetlib::kMatFlag_HasNormalMap) && fs::exists(g_dir / "out" / a.materials[1].normalMapPath),
              "an external texture, relative to the source file, is cooked to a sibling too (%s)", a.materials[1].normalMapPath);
        CHECK(!(a.materials[0].flags & assetlib::kMatFlag_HasNormalMap) && hasNote(c, "'missing.tga' could not be read"),
              "an unreadable texture leaves its slot empty and says so");
    }

    // ── 6. Skinned: skeleton, weights, clips ────────────────────────────────
    std::printf("6. skinned\n");
    {
        Cooked c = cook(impcontract::expected(Case::SkinnedColumn), "column");
        const auto& a = c.asset;
        CHECK(c.loaded && a.header.version == 6 && a.header.vertexStride == 68 && a.header.boneCount == 2,
              "skinned layout: v6, 68-byte vertices, 2 bones (%s)", c.result.error.c_str());
        const V68 mid = skv(a, 2);
        CHECK(near3(mid.px, mid.py, mid.pz, -0.1f, 0.5f, 0) && mid.j[0] == 0 && mid.j[1] == 1 &&
              near(mid.w[0], 0.5f) && near(mid.w[1], 0.5f), "mesh-space position and blended weights");
        CHECK(a.bones.size() == 2 && std::string(a.bones[1].name) == "Spine" && a.bones[1].parentIndex == 0 &&
              near(a.bones[1].localBindMatrix[13], 0.5f) && near(a.bones[1].inverseBindMatrix[13], -0.5f),
              "bones: names, parents, bind and inverse-bind matrices");
        CHECK(!a.skeletonBlob.empty() && a.clips.size() == 1 && a.clips[0].name == "Bend" &&
              a.clips[0].mappedTracks == 1 && a.clips[0].totalTracks == 1, "skeleton archive and one clip, 'Bend'");

        // Read the clip back and SAMPLE it: at the end, Spine is turned 45° about Z.
        // This is what proves the keys were not conjugated on the way in.
        ozz::animation::Skeleton skel; ozz::animation::Animation anim;
        { ozz::io::MemoryStream ms; ms.Write(a.skeletonBlob.data(), a.skeletonBlob.size()); ms.Seek(0, ozz::io::Stream::kSet);
          ozz::io::IArchive ar(&ms); ar >> skel; }
        { ozz::io::MemoryStream ms; ms.Write(a.clips[0].blob.data(), a.clips[0].blob.size()); ms.Seek(0, ozz::io::Stream::kSet);
          ozz::io::IArchive ar(&ms); ar >> anim; }
        ozz::animation::SamplingJob::Context sctx; sctx.Resize(skel.num_joints());
        std::vector<ozz::math::SoaTransform> locals((size_t)skel.num_soa_joints());
        ozz::animation::SamplingJob job;
        job.animation = &anim; job.context = &sctx; job.ratio = 1.0f; job.output = ozz::make_span(locals);
        int spine = -1;
        for (int j = 0; j < skel.num_joints(); ++j) if (std::string(skel.joint_names()[j]) == "Spine") spine = j;
        const bool ran = job.Run() && spine >= 0;
        float qz[4] = {}, qw[4] = {};
        if (ran) {
            ozz::math::StorePtrU(locals[(size_t)spine / 4].rotation.z, qz);
            ozz::math::StorePtrU(locals[(size_t)spine / 4].rotation.w, qw);
        }
        CHECK(ran && near(anim.duration(), 1.0f) && near(std::fabs(qz[spine % 4]), 0.38268343f, 1e-3f) &&
              near(std::fabs(qw[spine % 4]), 0.92387953f, 1e-3f) && qz[spine % 4] * qw[spine % 4] > 0,
              "the clip samples to Spine at +45° about Z at its end (z %.4f, w %.4f)", ran ? qz[spine % 4] : 0.f, ran ? qw[spine % 4] : 0.f);

        // A rotated bind bone: the engine's Bone keeps the CONJUGATE rotation.
        ImportedScene rot = impcontract::expected(Case::SkinnedColumn);
        Float4x4& L = rot.skeleton->bones[1].bindLocal;   // 90° about Z, then (0, 0.5, 0)
        L.m[0] = 0; L.m[1] = 1; L.m[4] = -1; L.m[5] = 0;
        Float4x4& I = rot.skeleton->bones[1].inverseBind; // inverse of T(0,0.5,0)·Rz(90) = Rz(-90)·T(0,-0.5,0)
        I = {}; I.m[0] = 0; I.m[1] = -1; I.m[4] = 1; I.m[5] = 0; I.m[12] = -0.5f; I.m[13] = 0;
        Cooked rc = cook(rot, "rotated_bone");
        CHECK(rc.loaded && near(rc.asset.bones[1].bindRotation[2], -0.70710678f, 1e-4f) &&
              near(rc.asset.bones[1].bindRotation[3], 0.70710678f, 1e-4f),
              "a bone turned +90° about Z is stored as its conjugate (z %.4f, w %.4f) (%s)",
              rc.loaded ? rc.asset.bones[1].bindRotation[2] : 0.f, rc.loaded ? rc.asset.bones[1].bindRotation[3] : 0.f,
              rc.result.error.c_str());
    }

    // ── 7. A mesh with no weights in a skinned scene: bound, not dropped ────
    std::printf("7. rigid meshes in a skinned scene\n");
    {
        ImportedScene s = impcontract::expected(Case::SkinnedColumn);
        s.meshes.push_back(impcontract::build::triangle());
        s.meshes[1].name = "Hat";
        s.nodes.push_back(impcontract::build::node("Spine", 0, impcontract::build::translation(0, 0.5f, 0), {}));
        s.nodes.push_back(impcontract::build::node("HatNode", 1, impcontract::build::translation(0, 0.5f, 0), {1}));
        Cooked c = cook(s, "hat");
        const auto& a = c.asset;
        CHECK(c.loaded && a.header.vertexCount == 9, "the rigid mesh is cooked, not dropped (%u vertices)", a.header.vertexCount);
        const V68 h = skv(a, 7);                          // the hat's (1,0,0), placed at y = 1
        CHECK(near3(h.px, h.py, h.pz, 1, 1, 0) && h.j[0] == 1 && near(h.w[0], 1),
              "placed by its nodes and bound wholly to the nearest ancestor bone, 'Spine'");
        CHECK(hasNote(c, "'Hat' has no weights: bound rigidly to bone 'Spine'"), "and it says so");
    }

    // ── 8. Less losses are logged; the cook is deterministic ────────────────
    std::printf("8. notes and determinism\n");
    {
        Cooked c = cook(impcontract::expected(Case::Unrepresentable), "less");
        CHECK(c.result.success && hasNote(c, "not cooked (morph targets)") && hasNote(c, "not cooked (camera)"),
              "Less losses cook, and each is reported");
        auto bytes = [](const fs::path& p) { std::ifstream f(p, std::ios::binary);
                                             return std::vector<char>(std::istreambuf_iterator<char>(f), {}); };
        cook(impcontract::expected(Case::SkinnedColumn), "det_a");
        cook(impcontract::expected(Case::SkinnedColumn), "det_b");
        CHECK(bytes(g_dir / "out" / "det_a.cooked") == bytes(g_dir / "out" / "det_b.cooked"),
              "the same scene cooks to the same bytes");
    }

    fs::remove_all(g_dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
