// anim_pose_test — numeric verification of the ozz animation path.
//
//   usage: anim_pose_test <character.fbx> <clip.fbx> [time]
//
// Loads the character skeleton exactly like the runtime (PRESERVE_PIVOTS=false
// + helper-chain collapse + ozz skeleton build), binds the standalone clip via
// ClipLibrary (ozz AnimationBuilder), samples with the SAME ozz jobs the
// AnimatorSystem runs, builds the skin palette, and CPU-skins every mesh
// vertex. Passing = bind palette ~identity AND the skinned-vertex AABB is a
// sane character (an exploded pose fails loudly). CPU-only, no window/bgfx —
// but remember: CPU-correct does not imply GPU-correct (see animation/info.md,
// palette layout contract); pair with an on-screen look when touching this.
#include <cstring>   // std::memcpy/std::strlen — libc++ pulls these in
                     // transitively, libstdc++ does not, so the Linux legs
                     // are where a missing one surfaces
#include "animation/assimp_skeleton_loader.h"
#include "animation/clip_library.h"
#include "animation/clip_registry.h"
#include "animation/ozz_bridge.h"
#include "animation/pose.h"

#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cmath>
#include <cstdio>
#include <vector>

static float len3(float x, float y, float z) { return std::sqrt(x*x + y*y + z*z); }

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: anim_pose_test <character.fbx> <clip.fbx> [time]\n"); return 2; }
    const float t = argc > 3 ? (float)atof(argv[3]) : 0.0f;

    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices | aiProcess_SortByPType);
    if (!scene) { std::fprintf(stderr, "FAIL: %s\n", imp.GetErrorString()); return 1; }

    Skeleton skel = anim::extractSkeleton(scene);
    if (!anim::buildOzzSkeleton(skel)) { std::fprintf(stderr, "FAIL: ozz skeleton\n"); return 1; }
    const int n = skel.boneCount();
    std::printf("skeleton: %d bones (ozz joints: %d)\n", n, skel.ozz->num_joints());

    AnimClipRegistry clips;
    ClipLibrary lib;
    AnimClipHandle h = lib.load(argv[2], SkeletonHandle{1}, skel, clips);
    if (!h.valid()) { std::fprintf(stderr, "FAIL: clip bind\n"); return 1; }
    const AnimClip& clip = *clips.get(h);

    // ── Bind palette must be ~identity (raw-matrix path) ─────────────────────
    std::vector<float> world((size_t)n * 16), skin((size_t)n * 16);
    anim::computeBindPoseWorldMatrices(skel, world.data());
    anim::computeSkinMatrices(skel, world.data(), skin.data());
    float bindMaxT = 0.0f;
    for (int i = 0; i < n; ++i)
        bindMaxT = std::max(bindMaxT, len3(skin[i*16+12], skin[i*16+13], skin[i*16+14]));
    std::printf("bind palette: max |skin translation| = %.3f (should be ~0)\n", bindMaxT);
    if (bindMaxT > 1.0f) { std::fprintf(stderr, "FAIL: bind palette not identity\n"); return 1; }

    // ── Sample via the SAME ozz jobs the AnimatorSystem runs ────────────────
    ozz::animation::SamplingJob::Context sctx(skel.ozz->num_joints());
    std::vector<ozz::math::SoaTransform> locals((size_t)skel.ozz->num_soa_joints());
    std::vector<ozz::math::Float4x4>     models((size_t)skel.ozz->num_joints());

    ozz::animation::SamplingJob sample;
    sample.animation = clip.ozz.get();
    sample.context   = &sctx;
    sample.ratio     = clip.duration > 0.0f ? t / clip.duration : 0.0f;
    sample.output    = ozz::make_span(locals);
    if (!sample.Run()) { std::fprintf(stderr, "FAIL: SamplingJob\n"); return 1; }

    ozz::animation::LocalToModelJob l2m;
    l2m.skeleton = skel.ozz.get();
    l2m.input    = ozz::make_span(locals);
    l2m.output   = ozz::make_span(models);
    if (!l2m.Run()) { std::fprintf(stderr, "FAIL: LocalToModelJob\n"); return 1; }

    for (int i = 0; i < n; ++i) {
        float model[16];
        const ozz::math::Float4x4& m = models[(size_t)skel.ozzJointOf[i]];
        for (int c = 0; c < 4; ++c) ozz::math::StorePtrU(m.cols[c], &model[c * 4]);
        std::memcpy(&world[(size_t)i * 16], model, sizeof(model));
        bx::mtxMul(&skin[(size_t)i * 16], skel.bones[i].inverseBindMatrix, model);
    }

    // Bone world AABB (posed humanoid sanity)
    {
        float mn[3] = {1e9f,1e9f,1e9f}, mx[3] = {-1e9f,-1e9f,-1e9f};
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], world[i*16+12+k]);
                mx[k] = std::max(mx[k], world[i*16+12+k]);
            }
        std::printf("bone world AABB t=%.2f: (%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n",
                    t, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    }

    // ── The decisive check: CPU-skin every mesh vertex ───────────────────────
    auto xform = [](const float* m, float x, float y, float z, float out[3]) {
        out[0] = x*m[0] + y*m[4] + z*m[8]  + m[12];   // row-vector v*M
        out[1] = x*m[1] + y*m[5] + z*m[9]  + m[13];
        out[2] = x*m[2] + y*m[6] + z*m[10] + m[14];
    };
    float mn[3] = {1e9f,1e9f,1e9f}, mx[3] = {-1e9f,-1e9f,-1e9f};
    int far = 0, total = 0;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* am = scene->mMeshes[mi];
        if (am->mNumBones == 0) continue;
        auto bd = anim::extractBoneWeights(am, skel);
        for (unsigned v = 0; v < am->mNumVertices; ++v) {
            float acc[3] = {0,0,0};
            for (int j = 0; j < 4; ++j) {
                if (bd[v].weights[j] <= 0.0f) continue;
                float p[3];
                xform(&skin[(size_t)bd[v].joints[j] * 16],
                      am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z, p);
                for (int k = 0; k < 3; ++k) acc[k] += p[k] * bd[v].weights[j];
            }
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], acc[k]); mx[k] = std::max(mx[k], acc[k]); }
            if (len3(acc[0], acc[1], acc[2]) > 500.0f) ++far;
            ++total;
        }
    }
    std::printf("CPU-skinned %d verts: AABB (%.1f %.1f %.1f)..(%.1f %.1f %.1f)  beyond 5m: %d\n",
                total, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], far);
    if (far > 0) { std::fprintf(stderr, "FAIL: %d exploded vertices\n", far); return 1; }

    std::printf("anim_pose_test: PASS — '%s' %.2fs via ozz\n", clip.name.c_str(), clip.duration);
    return 0;
}
