// ── real_gltf_test — a skinned, animated glTF from a real exporter (WO-036) ──
//
// Every other glTF the tests read is written by the tests themselves, so it can
// only be as wrong as the test's own idea of glTF. This one is not:
// tests/fixtures/gltf/CesiumMan.glb is the Khronos sample character, exported by
// COLLADA2GLTF (Cesium, CC-BY 4.0; see tests/fixtures/gltf/README.md). It is a
// file the WO-014 reader was not written against, and it has what the written
// fixtures do not: a Z-up conversion and an armature ABOVE the joints, a skinned
// mesh node whose own transform the spec says to ignore, 19 joints, and one
// unnamed clip with 57 channels.
//
// The reader is checked against a second opinion computed here, straight from
// the glTF spec with cgltf's own low-level helpers (accessor reads,
// cgltf_node_transform_world), none of the front end's code:
//   1. The file's facts: joint, channel and key-time counts.
//   2. Every vertex skinned at rest, Σ weight × jointWorld × inverseBind × p,
//      must be a corner the reader's scene skins to the same place; and he
//      stands up +Y, a person's height, feet on the ground.
//   3. Every joint's world position at the clip's first and last key, posed
//      from the file's own samplers, must be where the reader's clip puts it.
//   4. It cooks: MeshAsset v6, every bone, the clip with all its tracks mapped.
// (Assimp is not the second opinion: its glTF importer is not built, and
// turning it on would ship a second glTF parser to test the first.)
// The editor check (it walks, upright, the right way round) is the user's eye.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/import/frontend_cgltf.h"
#include "import_contract.h"
#include <assetlib/mesh_asset.h>
#include <cgltf.h>

namespace fs = std::filesystem;
using namespace imp;

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

static const fs::path kFile = fs::path(ENGINE_SOURCE_DIR) / "tests/fixtures/gltf/CesiumMan.glb";

int main() {
    std::printf("real_gltf_test: %s\n", kFile.string().c_str());

    // ── 1. The file's facts, read by cgltf directly ─────────────────────────
    cgltf_options o{}; cgltf_data* d = nullptr;
    const bool parsed = cgltf_parse_file(&o, kFile.string().c_str(), &d) == cgltf_result_success
                     && cgltf_load_buffers(&o, d, kFile.string().c_str()) == cgltf_result_success;
    CHECK(parsed && d->skins_count == 1 && d->animations_count == 1 && d->meshes_count == 1,
          "the fixture is there, with one skin, one animation and one mesh");
    if (!parsed) { if (d) cgltf_free(d); std::printf("real_gltf_test: 1 failure(s)\n"); return 1; }
    const cgltf_skin& skin = d->skins[0];
    const cgltf_animation& anim = d->animations[0];
    float fileDuration = 0;
    for (size_t k = 0; k < anim.samplers_count; ++k) fileDuration = std::max(fileDuration, anim.samplers[k].input->max[0]);

    // ── The cgltf front end ─────────────────────────────────────────────────
    const ImportResult cg = CgltfFrontend().importScene(kFile, {});
    CHECK(cg, "the cgltf front end reads it (%s)", cg ? "" : cg.error().message.c_str());
    if (!cg) { cgltf_free(d); std::printf("real_gltf_test: %d failure(s)\n", g_failures); return 1; }
    const ImportedScene& s = cg.scene();
    const std::vector<Violation> bad = checkScene(s);
    CHECK(bad.empty(), "the scene is well formed (%zu violation(s)%s%s)", bad.size(),
          bad.empty() ? "" : ": ", bad.empty() ? "" : (std::string(bad[0].check) + " " + bad[0].detail).c_str());

    bool wrong = false;
    for (const Dropped& dr : s.dropped) {
        std::printf("        dropped (%s): %s\n", dr.effect == Dropped::Effect::Wrong ? "wrong" : "less", dr.what.c_str());
        wrong |= dr.effect == Dropped::Effect::Wrong;
    }
    CHECK(!wrong, "nothing it drops is Wrong, so the cook is not refused");

    const size_t bones = s.skeleton ? s.skeleton->bones.size() : 0;
    // The joints, plus their ancestors Z_UP and Armature, plus the root: the
    // conversion above the armature is kept, not flattened away.
    CHECK(bones >= skin.joints_count + 2, "skeleton: %zu bone(s), the file's %zu joints and their ancestors", bones, skin.joints_count);
    CHECK(s.meshes.size() == 1 && s.meshes[0].weights.size() == s.meshes[0].positions.size(),
          "one mesh, weighted at every vertex");
    CHECK(s.clips.size() == 1 && s.clips[0].name == "CesiumMan", "one clip, named after the file (it is unnamed in it)");
    size_t tracks = 0;
    if (s.clips.size() == 1) for (const Track& t : s.clips[0].tracks)
        tracks += !t.translation.empty() + !t.rotation.empty() + !t.scale.empty();
    CHECK(tracks == anim.channels_count, "every one of the file's %zu channels is read (%zu)", anim.channels_count, tracks);
    CHECK(s.clips.size() == 1 && std::fabs(s.clips[0].duration - fileDuration) < 1e-4f,
          "the clip lasts %.3f s, as the file's key times say", fileDuration);

    // ── 2. Skinned at rest: the spec's sum, against the reader's scene ──────
    auto world = [](const cgltf_node* n) { Float4x4 m; cgltf_node_transform_world(n, m.m); return m; };
    auto key = [](Float3 v) { auto q = [](float f) { return (long)std::lround(f * 1000.0f); };  // 1 mm
                              return std::to_string(q(v.x)) + "," + std::to_string(q(v.y)) + "," + std::to_string(q(v.z)); };
    std::vector<Float4x4> jointSkin(skin.joints_count);          // jointWorld × inverseBind
    for (size_t j = 0; j < skin.joints_count; ++j) {
        Float4x4 ibm; cgltf_accessor_read_float(skin.inverse_bind_matrices, j, ibm.m, 16);
        jointSkin[j] = mul(world(skin.joints[j]), ibm);
    }
    std::set<std::string> fileRest;
    Float3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (size_t pi = 0; pi < d->meshes[0].primitives_count; ++pi) {
        const cgltf_primitive& prim = d->meshes[0].primitives[pi];
        const cgltf_accessor *P = nullptr, *J = nullptr, *W = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& at = prim.attributes[a];
            if (at.type == cgltf_attribute_type_position) P = at.data;
            if (at.type == cgltf_attribute_type_joints && at.index == 0) J = at.data;
            if (at.type == cgltf_attribute_type_weights && at.index == 0) W = at.data;
        }
        for (size_t v = 0; P && J && W && v < P->count; ++v) {
            Float3 p; cgltf_accessor_read_float(P, v, &p.x, 3);
            cgltf_uint j[4]; cgltf_accessor_read_uint(J, v, j, 4);
            float w[4]; cgltf_accessor_read_float(W, v, w, 4);
            Float3 r{0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const Float3 q = transformPoint(jointSkin[j[k]], p);
                r = {r.x + w[k] * q.x, r.y + w[k] * q.y, r.z + w[k] * q.z};
            }
            fileRest.insert(key(r));
            lo = {std::min(lo.x, r.x), std::min(lo.y, r.y), std::min(lo.z, r.z)};
            hi = {std::max(hi.x, r.x), std::max(hi.y, r.y), std::max(hi.z, r.z)};
        }
    }
    size_t corners = 0, strays = 0;
    std::set<std::string> readRest;
    for (const auto& t : impcontract::detail::soup(s))
        for (const auto& c : t.c) { ++corners; readRest.insert(key(c.rest)); strays += !fileRest.count(key(c.rest)); }
    CHECK(corners > 0 && strays == 0 && readRest.size() == fileRest.size(),
          "every vertex, skinned at rest, lands where the spec puts it (%zu of %zu corners off; %zu places, want %zu)",
          strays, corners, readRest.size(), fileRest.size());
    const float h = hi.y - lo.y;
    std::printf("        skinned at rest: %s to %s\n", impcontract::detail::str(lo).c_str(), impcontract::detail::str(hi).c_str());
    // In a T-pose, so his arm span is close to his height; lying down, or on his
    // face, the height would be along X or Z.
    CHECK(h > 1.2f && h < 2.0f && h >= hi.x - lo.x && h > 3 * (hi.z - lo.z) && std::fabs(lo.y) < 0.05f,
          "he stands up +Y, a person's height (%.2f m), feet on the ground", h);

    // ── 3. The clip: every joint where the file's samplers put it ──────────
    // Pose the file's own nodes from each channel's first (then last) key, and
    // ask cgltf for the joint's world origin.
    if (s.skeleton && s.clips.size() == 1) {
        size_t off = 0; std::string first;
        for (bool last : {false, true}) {
            for (size_t c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& ch = anim.channels[c];
                const size_t k = last ? ch.sampler->input->count - 1 : 0;
                cgltf_node* n = ch.target_node;
                if (ch.target_path == cgltf_animation_path_type_translation) { cgltf_accessor_read_float(ch.sampler->output, k, n->translation, 3); n->has_translation = 1; }
                if (ch.target_path == cgltf_animation_path_type_rotation)    { cgltf_accessor_read_float(ch.sampler->output, k, n->rotation, 4);    n->has_rotation = 1; }
                if (ch.target_path == cgltf_animation_path_type_scale)       { cgltf_accessor_read_float(ch.sampler->output, k, n->scale, 3);       n->has_scale = 1; }
            }
            const float t = last ? fileDuration : 0.0f;
            for (size_t j = 0; j < skin.joints_count; ++j) {
                const std::string nm = skin.joints[j]->name ? skin.joints[j]->name : "";
                const auto& bs = s.skeleton->bones;
                size_t bi = 0; while (bi < bs.size() && bs[bi].name != nm) ++bi;
                const Float3 want = transformPoint(world(skin.joints[j]), {0, 0, 0});
                const Float3 got = bi < bs.size() ? transformPoint(impcontract::detail::posedWorld(bs, s.clips[0], bi, t), {0, 0, 0}) : Float3{1e9f, 0, 0};
                if (!impcontract::detail::near3(got, want)) { if (!off++) first = nm + " at " + std::to_string(t) + " s: " + impcontract::detail::str(got) + ", want " + impcontract::detail::str(want); }
            }
        }
        CHECK(off == 0, "every joint, at the clip's first and last key, is where the file's samplers put it (%zu off%s%s)",
              off, off ? "; first: " : "", first.c_str());
    }
    cgltf_free(d);

    // ── 4. It cooks ─────────────────────────────────────────────────────────
    const fs::path out = fs::temp_directory_path() / "real_gltf_test_CesiumMan.cooked";
    fs::remove(out);
    MeshCooker cooker;
    assetlib::CookContext ctx; ctx.sourcePath = kFile; ctx.outputPath = out;
    const assetlib::CookResult r = cooker.cook(ctx);
    assetlib::MeshAsset a;
    const bool loaded = r.success && assetlib::loadMesh(a, out);
    CHECK(loaded, "it cooks (%s)", r.error.c_str());
    if (loaded) {
        CHECK(a.header.version == 6 && a.header.boneCount == bones && !a.skeletonBlob.empty(),
              "as MeshAsset v%u with %u bone(s) and a skeleton", a.header.version, a.header.boneCount);
        // The bounds are what the renderer culls with and what the editor's
        // spawn scales and grounds by, so they must be where he DRAWS: skinned
        // at rest, standing up +Y with his feet at 0. Bounds of the raw
        // bind-space (Z-up) vertices laid the box on its side and floated him.
        const float* bn = a.header.boundsMin; const float* bx = a.header.boundsMax;
        std::printf("        cooked bounds: (%.3f, %.3f, %.3f) to (%.3f, %.3f, %.3f)\n", bn[0], bn[1], bn[2], bx[0], bx[1], bx[2]);
        CHECK(std::fabs(bn[0] - lo.x) < 1e-3f && std::fabs(bn[1] - lo.y) < 1e-3f && std::fabs(bn[2] - lo.z) < 1e-3f &&
              std::fabs(bx[0] - hi.x) < 1e-3f && std::fabs(bx[1] - hi.y) < 1e-3f && std::fabs(bx[2] - hi.z) < 1e-3f,
              "the cooked bounds are his skinned-at-rest box, feet at y = %.3f", bn[1]);
        CHECK(a.clips.size() == 1 && a.clips[0].mappedTracks == a.clips[0].totalTracks && a.clips[0].mappedTracks > 0,
              "one clip, all %d track(s) mapped to bones", a.clips.empty() ? 0 : a.clips[0].totalTracks);
    }
    fs::remove(out);

    std::printf("real_gltf_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
