// ── animator_system_test — AnimatorSystem's own behavior ────────────────────
//
// The animation MATH was covered (anim_pose_test, clip_binding_test) but the
// ECS system that drives it was not: clip advance, looping, clamp-and-stop,
// speed scale, the bind-pose fallback, and — the reason this file exists — the
// `boneCount > kMaxBones` guard.
//
// That guard is a SILENT-CORRUPTION path. computeBindPosePalette declares
// `float worldMatrices[kMaxBones * 16]` on the stack and SkinnedMesh carries a
// fixed `skinMatrices[128 * 16]`; a 129-bone skeleton that slipped past the
// check would smash both. Nothing would report an error — it would corrupt the
// stack and render garbage. So the contract under test is not "it returns
// false", it is "an oversized skeleton NEVER writes a palette".
//
// Hermetic: builds skeletons and clips directly through ozz's offline builders,
// so it needs no Assimp, no asset files, and no GPU. Unit lane.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <flecs.h>

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>

#include "animation/clip_registry.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton_registry.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"
#include "runtime/jobs/jobs.h"
#include "systems/animator_system.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// ── Fixtures ────────────────────────────────────────────────────────────────

// A flat skeleton: bone 0 is the root, every other bone is its child. All bind
// transforms are identity, so bind-pose world matrices are identity and, with
// identity inverse-bind matrices, every skin matrix must come out identity —
// which makes "did the palette get written correctly?" a trivial assertion.
static Skeleton makeSkeleton(int boneCount) {
    Skeleton s;
    s.bones.resize((size_t)boneCount);
    for (int i = 0; i < boneCount; ++i) {
        s.bones[(size_t)i].name        = "b" + std::to_string(i);
        s.bones[(size_t)i].parentIndex = (i == 0) ? -1 : 0;
    }
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);   // fills s.ozz + s.ozzJointOf
    return s;
}

// A clip of the given duration with empty tracks: ozz treats a jointless track
// as "hold the rest pose", which is all this test needs — we are exercising the
// SYSTEM's time/palette logic, not ozz's interpolation (covered elsewhere).
static AnimClip makeClip(const Skeleton& skel, float duration) {
    AnimClip clip;
    if (!skel.ozz) return clip;

    ozz::animation::offline::RawAnimation raw;
    raw.duration = duration;
    raw.tracks.resize((size_t)skel.ozz->num_joints());

    ozz::animation::offline::AnimationBuilder builder;
    ozz::unique_ptr<ozz::animation::Animation> built = builder(raw);
    if (!built) return clip;

    clip.name     = "test_clip";
    clip.duration = duration;
    clip.ozz      = std::shared_ptr<const ozz::animation::Animation>(
        built.release(), ozz::Deleter<ozz::animation::Animation>());
    return clip;
}

// One entity + the system, wired to registries. Returned by value so each case
// starts from a clean world (per-entity ozz contexts are keyed by entity id).
struct Fixture {
    flecs::world      ecs;
    SkeletonRegistry  skeletons;
    AnimClipRegistry  clips;
    AnimatorSystem    sys;
    flecs::entity     e;

    void build(const Skeleton& skel, const AnimClip* clip, Animator animInit) {
        sys.init(ecs, skeletons, clips);
        SkinnedMesh sm{};
        sm.skeleton = skeletons.add(Skeleton(skel));
        if (clip && clip->valid())
            animInit.clip = clips.add(AnimClip(*clip));
        e = ecs.entity().set<Animator>(animInit).set<SkinnedMesh>(sm);
    }
    void tick(float dt) { sys.tick(dt); }
    const Animator&    anim() const { return e.get<Animator>(); }
    const SkinnedMesh& skin() const { return e.get<SkinnedMesh>(); }
};

static bool isIdentity(const float* m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float want = (r == c) ? 1.0f : 0.0f;
            if (std::fabs(m[r * 4 + c] - want) > 1e-4f) return false;
        }
    return true;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("animator_system_test: AnimatorSystem behavior\n");
    jobs::init();   // stepAll dispatches through jobs::parallelFor

    // ── 1. The kMaxBones guard — the silent-corruption path ─────────────────
    // A 129-bone skeleton must never produce a palette, on BOTH routes into
    // the writer: the clip path (guarded in step()) and the no-clip bind-pose
    // path (guarded in computeBindPosePalette, which is where the fixed-size
    // stack array lives). Under ASan this case is also the overflow detector.
    {
        Skeleton big = makeSkeleton(kMaxBones + 1);
        CHECK(big.boneCount() == 129 && big.ozz != nullptr,
              "oversized skeleton builds (%d bones, ozz ok)", big.boneCount());

        Fixture f;                                   // no clip -> bind-pose route
        f.build(big, nullptr, Animator{});
        f.tick(0.016f);
        CHECK(!f.skin().hasSkinMatrices,
              "129 bones, no clip -> NO palette written (stack array is 128)");

        AnimClip clip = makeClip(big, 1.0f);
        Fixture g;                                   // clip -> sampling route
        Animator a{}; a.playing = true; a.looping = true;
        g.build(big, &clip, a);
        g.tick(0.016f);
        CHECK(!g.skin().hasSkinMatrices,
              "129 bones, with clip -> NO palette written");
    }

    // ── 2. A legal skeleton at the boundary still works ──────────────────────
    // The guard is `>` kMaxBones, so exactly 128 must be accepted — an
    // off-by-one here would silently disable skinning on max-size rigs.
    {
        Skeleton edge = makeSkeleton(kMaxBones);
        Fixture f;
        f.build(edge, nullptr, Animator{});
        f.tick(0.016f);
        CHECK(f.skin().hasSkinMatrices,
              "exactly 128 bones -> palette IS written (guard is >, not >=)");
        CHECK(isIdentity(&f.skin().skinMatrices[0])
                  && isIdentity(&f.skin().skinMatrices[127 * 16]),
              "bind-pose palette is identity at first and last bone");
    }

    // ── 3. Bind-pose fallback when there is no clip ─────────────────────────
    {
        Skeleton s = makeSkeleton(4);
        Fixture f;
        f.build(s, nullptr, Animator{});
        f.tick(0.016f);
        CHECK(f.skin().hasSkinMatrices, "no clip -> bind-pose palette written");
        CHECK(isIdentity(&f.skin().skinMatrices[2 * 16]),
              "bind-pose skin matrix is identity (IBM * bind world)");
    }

    // ── 4. Missing skeleton must not write a palette ────────────────────────
    {
        Fixture f;
        f.sys.init(f.ecs, f.skeletons, f.clips);
        SkinnedMesh sm{};                       // default handle = no skeleton
        sm.hasSkinMatrices = true;              // pretend a stale palette
        f.e = f.ecs.entity().set<Animator>(Animator{}).set<SkinnedMesh>(sm);
        f.tick(0.016f);
        CHECK(!f.skin().hasSkinMatrices,
              "invalid skeleton handle -> palette flag CLEARED, not left stale");
    }

    // ── 5. Time advance honours speed ───────────────────────────────────────
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 10.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.speed = 2.0f;
        f.build(s, &c, a);
        f.tick(0.1f);
        CHECK(std::fabs(f.anim().time - 0.2f) < 1e-5f,
              "time += dt * speed (0.1 * 2.0 = %.3f)", f.anim().time);
    }

    // ── 6. Paused animators do not advance ──────────────────────────────────
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 10.0f);
        Fixture f;
        Animator a{}; a.playing = false; a.time = 3.0f;
        f.build(s, &c, a);
        f.tick(0.5f);
        CHECK(std::fabs(f.anim().time - 3.0f) < 1e-6f,
              "playing=false -> time frozen (%.3f)", f.anim().time);
        CHECK(f.skin().hasSkinMatrices,
              "paused still samples a pose (frozen, not blank)");
    }

    // ── 7. Looping wraps instead of running past the end ────────────────────
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.time = 0.9f;
        f.build(s, &c, a);
        f.tick(0.25f);                       // 0.9 + 0.25 = 1.15 -> wraps to 0.15
        CHECK(std::fabs(f.anim().time - 0.15f) < 1e-5f,
              "looping wraps via fmod (%.3f)", f.anim().time);
        CHECK(f.anim().playing, "looping clip keeps playing after the wrap");
    }

    // ── 8. Non-looping clamps at the end AND stops ──────────────────────────
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = false; a.time = 0.9f;
        f.build(s, &c, a);
        f.tick(0.5f);                        // would land at 1.4
        CHECK(std::fabs(f.anim().time - 1.0f) < 1e-5f,
              "non-looping clamps to duration (%.3f)", f.anim().time);
        CHECK(!f.anim().playing, "non-looping auto-stops at the end");
        CHECK(f.skin().hasSkinMatrices, "final pose still written after stop");
    }

    // ── 9. Reverse playback clamps at zero AND stops ────────────────────────
    // The negative branch is easy to get wrong (fmod of a negative would give
    // a negative ratio and ozz would reject the sample).
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = false; a.speed = -1.0f;
        a.time = 0.2f;
        f.build(s, &c, a);
        f.tick(0.5f);                        // would land at -0.3
        CHECK(std::fabs(f.anim().time) < 1e-6f,
              "reverse clamps to 0 (%.3f)", f.anim().time);
        CHECK(!f.anim().playing, "reverse auto-stops at the start");
    }

    // ── 10. Reverse + looping wraps to a POSITIVE time ──────────────────────
    // fmod(-0.1, 1.0) is -0.1 in C; the system must add duration back, or the
    // sampling ratio goes negative and the pose silently stops updating.
    {
        Skeleton s = makeSkeleton(4);
        AnimClip  c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.speed = -1.0f;
        a.time = 0.1f;
        f.build(s, &c, a);
        f.tick(0.3f);                        // 0.1 - 0.3 = -0.2 -> wraps to 0.8
        CHECK(f.anim().time >= 0.0f && std::fabs(f.anim().time - 0.8f) < 1e-5f,
              "reverse loop wraps to positive time (%.3f)", f.anim().time);
        CHECK(f.skin().hasSkinMatrices,
              "negative wrap still produces a pose (ratio stayed in range)");
    }

    jobs::shutdown();
    if (g_failures) {
        std::printf("animator_system_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("animator_system_test: PASS\n");
    return 0;
}
