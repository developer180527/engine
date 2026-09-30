// ── clip_cook_test — a clip nobody played still ships (WO-016) ────────────────
//
// A standalone clip (a Mixamo clip FBX) used to be cooked only when the editor
// first BOUND it, into a cache keyed by the target skeleton. A shipped build then
// had exactly the clips someone had happened to play, and failed "clip not
// cooked" on the rest.
//
// Now an animation-only source cooks, in the normal pipeline, to a
// skeleton-independent clip (animation/cooked_clip.h), and ClipLibrary binds it
// to a character at load. This runs the real pipeline over a two-file project:
// a skinned character and a separate clip that NOTHING binds before packaging.
//   1. both cook; the clip as a clip, not skipped
//   2. the editor's path: ClipLibrary with the registry locator and NO source
//      reader binds the cooked clip, and it poses exactly as the clip built
//      straight from the source does
//   3. the shipped path: packaging names it; a ClipLibrary with no registry and
//      no reader (engine_player) finds it by the source path a scene stores,
//      and loading writes nothing
//   4. a damaged cooked clip is refused with a reason, never decoded
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <assetlib/asset_registry.h>
#include <assetlib/cook_pipeline.h>
#include <assetlib/mesh_asset.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include "animation/clip_library.h"
#include "animation/clip_registry.h"
#include "animation/cooked_clip.h"
#include "animation/cooked_skin.h"
#include "assets/anim_from_scene.h"
#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/import/frontend_cgltf.h"
#include "gltf_writer.h"
#include "import_contract.h"
#include "tools/packaging/package_closure.h"

namespace fs = std::filesystem;
using impcontract::Case;

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

// Every local transform float of `clip` on `skel`, at several times.
static std::vector<float> pose(const AnimClip& clip, const Skeleton& skel) {
    std::vector<float> out;
    ozz::animation::SamplingJob::Context ctx(clip.ozz->num_tracks());
    std::vector<ozz::math::SoaTransform> local((size_t)skel.ozz->num_soa_joints());
    for (int s = 0; s <= 8; ++s) {
        ozz::animation::SamplingJob job;
        job.animation = clip.ozz.get(); job.context = &ctx; job.ratio = s / 8.0f; job.output = ozz::make_span(local);
        if (!job.Run()) return {};
        const float* f = reinterpret_cast<const float*>(local.data());
        out.insert(out.end(), f, f + local.size() * sizeof(ozz::math::SoaTransform) / sizeof(float));
    }
    return out;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("clip_cook_test\n");
    const fs::path root = fs::temp_directory_path() / "wo016_clip_cook";
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    { std::ofstream(root / "project.json") << "{\"name\":\"clips\"}"; }

    // The character: the skinned column, with its own clip.
    { std::ofstream(root / "assets" / "character.gltf") << gltfw::write(impcontract::expected(Case::SkinnedColumn)); }
    // The standalone clip: the same rig, animation only, a different clip.
    imp::ImportedScene walk = impcontract::expected(Case::SkinnedColumn);
    walk.meshes.clear(); walk.materials.clear();
    for (auto& n : walk.nodes) n.meshes.clear();
    walk.clips[0].name = "Walk";
    walk.clips[0].tracks[0].rotation[1].value = {0, 0, -0.38268343f, 0.92387953f};   // bends the other way
    { std::ofstream(root / "assets" / "walk.gltf") << gltfw::write(walk); }

    // ── 1. The real pipeline cooks both ─────────────────────────────────────
    std::printf("1. cook\n");
    const fs::path cache = root / ".cache";
    fs::create_directories(cache);
    assetlib::AssetRegistry reg;
    CHECK(reg.open(cache / "registry.db"), "a registry");
    reg.scan(root / "assets", root);
    assetlib::CookPipeline pipe(reg, root, cache);
    pipe.registerCooker(std::make_unique<MeshCooker>());
    pipe.cookAll();
    const auto chr = reg.findBySourcePath("assets/character.gltf");
    const auto clp = reg.findBySourcePath("assets/walk.gltf");
    CHECK(chr && chr->state == assetlib::AssetState::Ready && !chr->cookedPath.empty(), "the character cooks");
    anim::CookedClip cooked; std::string why;
    const bool clipCooked = clp && clp->state == assetlib::AssetState::Ready && !clp->cookedPath.empty() &&
                            anim::readCookedClipFile(cache / clp->cookedPath, cooked, why);
    CHECK(clipCooked, "the clip cooks, as a clip, though nothing ever bound it (%s%s)",
          clp ? clp->errorMessage.c_str() : "no record", why.c_str());
    if (!chr || !clipCooked) { std::printf("clip_cook_test: %d failure(s)\n", g_failures); return 1; }

    assetlib::MeshAsset mesh;
    CHECK(assetlib::loadMesh(mesh, cache / chr->cookedPath), "the character's cooked mesh loads");
    Skeleton skel = anim::decodeCookedSkeleton(mesh);
    CHECK(skel.ozz != nullptr, "and its skeleton decodes");
    if (!skel.ozz) { std::printf("clip_cook_test: %d failure(s)\n", g_failures); return 1; }

    // The reference: the clip built straight from the source, on the same skeleton.
    const imp::ImportResult src = imp::CgltfFrontend().importScene(root / "assets" / "walk.gltf", {});
    const AnimClip direct = src && !src.scene().clips.empty() ? imp::buildOzzClip(src.scene().clips[0], skel) : AnimClip{};
    CHECK(direct.valid(), "the reference clip builds from the source");

    // ── 2. The editor: the registry locates the cooked clip; no source reader ─
    std::printf("2. the editor's path\n");
    {
        ClipLibrary lib;
        lib.setCacheRoot(cache / "anim");
        lib.setCookedLocator([&](const std::string& s) -> fs::path {
            const auto r = reg.findBySourcePath(fs::relative(s, root).generic_string());
            return r && !r->cookedPath.empty() ? cache / r->cookedPath : fs::path{};
        });
        AnimClipRegistry clips;
        const AnimClipHandle h = lib.load((root / "assets" / "walk.gltf").string(), SkeletonHandle{1}, skel, clips);
        const AnimClip* c = h.valid() ? clips.get(h) : nullptr;
        CHECK(c && c->valid() && c->name == "Walk" && c->mappedTracks == 1 && c->totalTracks == 1,
              "the cooked clip binds by bone name, with no source reader ('%s', %d/%d)",
              c ? c->name.c_str() : "-", c ? c->mappedTracks : 0, c ? c->totalTracks : 0);
        CHECK(c && direct.valid() && pose(*c, skel) == pose(direct, skel),
              "and poses exactly as the clip built straight from the source (every float, 9 times)");
        CHECK(!fs::exists(cache / "anim"), "binding wrote nothing: there is no cook-on-first-bind cache");
    }

    // ── 3. A shipped build: no registry, no reader, only the package ─────────
    std::printf("3. the shipped path\n");
    {
        const fs::path dist = root / "dist";
        const auto files = pkg::packagedClips(reg, cache);
        CHECK(files.size() == 1 && files[0].sourcePath == "assets/walk.gltf",
              "packaging finds exactly the one cooked clip (%zu)", files.size());
        fs::create_directories(dist / ".cache" / "anim");
        for (const auto& f : files) fs::copy_file(cache / f.cookedRel, dist / ".cache" / "anim" / f.packagedName);

        ClipLibrary lib;                                    // engine_player: cache root only
        lib.setCacheRoot(dist / ".cache" / "anim");
        AnimClipRegistry clips;
        const AnimClipHandle h = lib.load("assets/walk.gltf", SkeletonHandle{1}, skel, clips);   // as a cooked scene names it
        const AnimClip* c = h.valid() ? clips.get(h) : nullptr;
        CHECK(c && c->valid() && pose(*c, skel) == pose(direct, skel),
              "a shipped build plays the clip nobody played in the editor, found by its source path");
        const AnimClipHandle a = lib.load((dist / "assets" / "walk.gltf").string(), SkeletonHandle{2}, skel, clips);
        CHECK(a.valid(), "and by an absolute path inside the package too");
        const AnimClipHandle none = lib.load("assets/never_cooked.fbx", SkeletonHandle{1}, skel, clips);
        CHECK(!none.valid(), "a clip that was never cooked is refused (\"clip not cooked\"), not parsed");
    }

    // ── 4. A damaged cooked clip is refused, not decoded ────────────────────
    std::printf("4. damage\n");
    {
        std::ifstream in(cache / clp->cookedPath, std::ios::binary);
        const std::vector<uint8_t> good((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        anim::CookedClip out;
        std::vector<uint8_t> flipped = good; flipped[flipped.size() / 2] ^= 0x40;
        CHECK(!anim::decodeCookedClip(flipped.data(), flipped.size(), out, why) && why.find("digest") != std::string::npos,
              "one flipped byte: refused by the digest before ozz reads it (%s)", why.c_str());
        size_t refused = 0;
        for (size_t n = 0; n < good.size(); ++n) refused += !anim::decodeCookedClip(good.data(), n, out, why);
        CHECK(refused == good.size(), "every truncation of the file is refused (%zu of %zu)", refused, good.size());
        std::vector<uint8_t> v2 = good; v2[4] = 2;
        CHECK(!anim::decodeCookedClip(v2.data(), v2.size(), out, why) && why.find("version") != std::string::npos,
              "a future version is refused by name (%s)", why.c_str());
    }

    fs::remove_all(root);
    std::printf("clip_cook_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
