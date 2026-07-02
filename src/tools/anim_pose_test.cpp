// anim_pose_test — numeric diagnosis of standalone-clip sampling.
//
//   usage: anim_pose_test <character.fbx> <clip.fbx> [time]
//
// Loads the character skeleton exactly like the runtime (PRESERVE_PIVOTS=false
// + helper-chain collapse), binds the standalone clip, samples a pose, and
// computes world + skin matrices with the SAME pose.h code the AnimatorSystem
// uses. Prints magnitude stats: a healthy skin palette has bounded
// translations (same order as the mesh's own units) — an "exploded mesh" shows
// up here as huge skin translations, and the per-bone dump names the first
// bones where the sampled local diverges from bind.
#include "animation/assimp_skeleton_loader.h"
#include "animation/clip_library.h"
#include "animation/clip_registry.h"
#include "animation/pose.h"

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
    const int n = skel.boneCount();
    std::printf("skeleton: %d bones\n", n);

    AnimClipRegistry clips;
    ClipLibrary lib;
    AnimClipHandle h = lib.load(argv[2], SkeletonHandle{1}, skel, clips);
    if (!h.valid()) { std::fprintf(stderr, "FAIL: clip bind\n"); return 1; }
    const AnimClip& clip = *clips.get(h);

    // ── Reference: bind pose must give skin ≈ identity ──────────────────────
    std::vector<float> world((size_t)n * 16), skin((size_t)n * 16);
    anim::computeBindPoseWorldMatrices(skel, world.data());
    anim::computeSkinMatrices(skel, world.data(), skin.data());
    float bindMaxT = 0.0f;
    for (int i = 0; i < n; ++i)
        bindMaxT = std::max(bindMaxT, len3(skin[i*16+12], skin[i*16+13], skin[i*16+14]));
    std::printf("bind pose:   max |skin translation| = %8.3f   (should be ~0)\n", bindMaxT);

    // ── Sampled pose at t ────────────────────────────────────────────────────
    Pose pose = anim::sampleClip(clip, skel, t);
    anim::computeWorldMatrices(skel, pose, world.data());
    anim::computeSkinMatrices(skel, world.data(), skin.data());
    float maxT = 0.0f; int maxTi = -1;
    for (int i = 0; i < n; ++i) {
        float tr = len3(skin[i*16+12], skin[i*16+13], skin[i*16+14]);
        if (tr > maxT) { maxT = tr; maxTi = i; }
    }
    std::printf("sampled t=%.2f: max |skin translation| = %8.3f   at bone '%s'\n",
                t, maxT, maxTi >= 0 ? skel.bones[maxTi].name.c_str() : "?");

    // Sanity: bone WORLD positions must form a plausible humanoid (bounded by
    // roughly the character's own extents) — an exploded pose shows up here.
    {
        bx::Vec3 mn{ 1e9f, 1e9f, 1e9f }, mx{ -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < n; ++i) {
            bx::Vec3 p{ world[i*16+12], world[i*16+13], world[i*16+14] };
            mn = bx::min(mn, p); mx = bx::max(mx, p);
        }
        std::printf("bone world AABB: (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n",
                    mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
    }

    // ── Per-bone: where does sampled local diverge from bind? ───────────────
    Pose bind = anim::bindPose(skel);
    std::printf("\nlargest local-ROTATION divergences (sampled vs bind, degrees):\n");
    struct Div { float deg; int i; };
    std::vector<Div> divs;
    for (int i = 0; i < n; ++i) {
        if (!(pose.animated[i] & 2)) continue;   // rotation-animated bones
        // angle between quats: 2*acos(|dot|)
        const bx::Quaternion& a = bind.locals[i].rotation;
        const bx::Quaternion& b = pose.locals[i].rotation;
        float d = std::fabs(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
        if (d > 1.0f) d = 1.0f;
        divs.push_back({ 2.0f * std::acos(d) * 57.2958f, i });
    }
    std::sort(divs.begin(), divs.end(), [](const Div& a, const Div& b){ return a.deg > b.deg; });
    int over30 = 0;
    for (const auto& dv : divs) if (dv.deg > 30.0f) ++over30;
    std::printf("  rotation-animated bones: %zu, over 30 deg from bind: %d\n",
                divs.size(), over30);
    for (size_t k = 0; k < divs.size() && k < 10; ++k) {
        int i = divs[k].i;
        std::printf("  %6.1f deg  %-34s bindQ(%.3f %.3f %.3f %.3f) sampQ(%.3f %.3f %.3f %.3f)\n",
            divs[k].deg, skel.bones[i].name.c_str(),
            bind.locals[i].rotation.x, bind.locals[i].rotation.y,
            bind.locals[i].rotation.z, bind.locals[i].rotation.w,
            pose.locals[i].rotation.x, pose.locals[i].rotation.y,
            pose.locals[i].rotation.z, pose.locals[i].rotation.w);
    }
    // ── CPU-skin the real mesh: the decisive vertex-level check ─────────────
    // Transforms every vertex exactly as vs_skinned does (LBS with the same
    // weights/indices the runtime uploads). A sane result = all CPU data is
    // correct and any remaining visual explosion is GPU-side.
    {
        auto xform = [](const float* m, float x, float y, float z, float out[3]) {
            out[0] = x*m[0] + y*m[4] + z*m[8]  + m[12];   // row-vector v*M
            out[1] = x*m[1] + y*m[5] + z*m[9]  + m[13];
            out[2] = x*m[2] + y*m[6] + z*m[10] + m[14];
        };
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        float wsMin = 1e9f, wsMax = -1e9f;
        int   far = 0, total = 0;
        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
            const aiMesh* am = scene->mMeshes[mi];
            if (am->mNumBones == 0) continue;
            auto bd = anim::extractBoneWeights(am, skel);
            for (unsigned v = 0; v < am->mNumVertices; ++v) {
                float ws = bd[v].weights[0] + bd[v].weights[1]
                         + bd[v].weights[2] + bd[v].weights[3];
                wsMin = std::min(wsMin, ws); wsMax = std::max(wsMax, ws);
                float acc[3] = { 0, 0, 0 };
                for (int j = 0; j < 4; ++j) {
                    if (bd[v].weights[j] <= 0.0f) continue;
                    float p[3];
                    xform(&skin[(size_t)bd[v].joints[j] * 16],
                          am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z, p);
                    acc[0] += p[0] * bd[v].weights[j];
                    acc[1] += p[1] * bd[v].weights[j];
                    acc[2] += p[2] * bd[v].weights[j];
                }
                for (int k = 0; k < 3; ++k) {
                    mn[k] = std::min(mn[k], acc[k]);
                    mx[k] = std::max(mx[k], acc[k]);
                }
                if (len3(acc[0], acc[1], acc[2]) > 500.0f) ++far;
                ++total;
            }
        }
        std::printf("CPU-skinned %d verts: AABB (%.1f %.1f %.1f)..(%.1f %.1f %.1f)  "
                    "weightSum[%.3f..%.3f]  verts beyond 5m: %d\n",
                    total, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
                    wsMin, wsMax, far);
    }

    // ── SQT round-trip check: toMatrix(bind SQT) vs localBindMatrix ─────────
    // If these diverge, the SQT path is broken for this rig even at bind —
    // every animated bone inherits the error.
    std::printf("\nSQT round-trip error (bind SQT -> matrix vs raw localBindMatrix):\n");
    struct MErr { float e; int i; };
    std::vector<MErr> merrs;
    for (int i = 0; i < n; ++i) {
        float m[16];
        bind.locals[i].toMatrix(m);
        float e = 0.0f;
        for (int k = 0; k < 16; ++k)
            e = std::max(e, std::fabs(m[k] - skel.bones[i].localBindMatrix[k]));
        merrs.push_back({ e, i });
    }
    std::sort(merrs.begin(), merrs.end(), [](const MErr& a, const MErr& b){ return a.e > b.e; });
    for (size_t k = 0; k < merrs.size() && k < 6; ++k) {
        int i = merrs[k].i;
        const float* d = skel.bones[i].localBindMatrix;
        float m[16]; bind.locals[i].toMatrix(m);
        std::printf("  err=%8.4f  %-30s\n", merrs[k].e, skel.bones[i].name.c_str());
        std::printf("    raw row0(%7.3f %7.3f %7.3f)  row3(%8.2f %8.2f %8.2f)\n",
                    d[0], d[1], d[2], d[12], d[13], d[14]);
        std::printf("    sqt row0(%7.3f %7.3f %7.3f)  row3(%8.2f %8.2f %8.2f)\n",
                    m[0], m[1], m[2], m[12], m[13], m[14]);
    }
    return 0;
}
