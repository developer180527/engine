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
#include "animation/skin_palette.h"
#include "test_watchdog.h"

// The palette moved out of the component into anim::skinPalettes(); the tests
// assert on its CONTENTS, so they resolve the slot the same way the renderer
// does. Returns a zero block for an unacquired slot so a failed assert reads as
// "not identity" rather than crashing.
static const float* palette(const SkinnedMesh& s) {
    static const float kNone[SkinnedMesh::kMatrixSize] = {};
    const float* p = anim::skinPalettes().at(s.paletteSlot);
    return p ? p : kNone;
}

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
    // Watchdog + phase markers: this test SEGFAULTS on Windows x64 (arm64 passes)
    // somewhere after section 4, and a crash reports no line number. With
    // unbuffered output the last phase printed brackets the section that died —
    // the same instrumentation that located input_test's hang in one run.
    testwd::begin("animator_system_test: AnimatorSystem behavior", 60);
    jobs::init();   // stepAll dispatches through jobs::parallelFor

    testwd::phase("1. The kMaxBones guard — the silent-corruption path");

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

    testwd::phase("2. A legal skeleton at the boundary still works");

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
        CHECK(isIdentity(&palette(f.skin())[0])
                  && isIdentity(&palette(f.skin())[127 * 16]),
              "bind-pose palette is identity at first and last bone");
    }

    testwd::phase("3. Bind-pose fallback when there is no clip");

    // ── 3. Bind-pose fallback when there is no clip ─────────────────────────
    {
        Skeleton s = makeSkeleton(4);
        Fixture f;
        f.build(s, nullptr, Animator{});
        f.tick(0.016f);
        CHECK(f.skin().hasSkinMatrices, "no clip -> bind-pose palette written");
        CHECK(isIdentity(&palette(f.skin())[2 * 16]),
              "bind-pose skin matrix is identity (IBM * bind world)");
    }

    testwd::phase("4. Missing skeleton must not write a palette");

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

    testwd::phase("5. Time advance honours speed");

    // ── 5. Time advance honours speed ───────────────────────────────────────
    {
        // Sub-phases: this is the FIRST section that actually samples a clip
        // with time advancing (1 and 2 bail on the bone guard, 3 is bind-pose,
        // 4 has no skeleton), and it segfaults on Windows x64 while arm64
        // passes — the signature of SSE's aligned loads faulting where NEON
        // tolerates. Narrowing to a statement beats guessing at which ozz job.
        testwd::phase("5a. makeSkeleton(4)");
        Skeleton s = makeSkeleton(4);
        testwd::phase("5b. makeClip");
        AnimClip  c = makeClip(s, 10.0f);
        testwd::phase("5c. Fixture ctor");
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.speed = 2.0f;
        testwd::phase("5d. f.build(skeleton, clip, animator)");
        f.build(s, &c, a);
        testwd::phase("5e. f.tick(0.1) — ozz sampling + LocalToModel");
        f.tick(0.1f);
        testwd::phase("5f. tick returned");
        CHECK(std::fabs(f.anim().time - 0.2f) < 1e-5f,
              "time += dt * speed (0.1 * 2.0 = %.3f)", f.anim().time);
    }

    testwd::phase("6. Paused animators do not advance");

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

    testwd::phase("7. Looping wraps instead of running past the end");

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

    testwd::phase("8. Non-looping clamps at the end AND stops");

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

    testwd::phase("9. Reverse playback clamps at zero AND stops");

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

    testwd::phase("10. Reverse + looping wraps to a POSITIVE time");

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

    testwd::phase("11. A clip change starts a crossfade");

    // ── 11. A clip change starts a crossfade ────────────────────────────────
    // The crossfade path had NO coverage anywhere — not in this file, and not
    // in the determinism gate, whose animator tier never changes a clip. These
    // four sections are the first time it runs under test at all.
    {
        Skeleton s = makeSkeleton(4);
        AnimClip cA = makeClip(s, 2.0f), cB = makeClip(s, 3.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.fade = 0.5f;
        f.build(s, &cA, a);
        const AnimClipHandle hB = f.clips.add(AnimClip(cB));
        f.tick(0.1f);                          // establishes the last-seen clip
        AnimatorSystem::FadeState fs;
        CHECK(f.sys.fadeState(f.e.id(), fs) && !fs.active,
              "no crossfade while the clip is unchanged");
        f.e.get_mut<Animator>().clip = hB;
        f.tick(0.1f);
        CHECK(f.sys.fadeState(f.e.id(), fs) && fs.active,
              "changing the clip starts a crossfade");
        CHECK(std::fabs(fs.duration - 0.5f) < 1e-6f,
              "lasting Animator::fade seconds (%.3f)", fs.duration);
        CHECK(std::fabs(fs.elapsed - 0.1f) < 1e-6f,
              "and one tick of it has elapsed (%.3f)", fs.elapsed);
    }

    testwd::phase("12. Sampling never moves the crossfade clocks");

    // ── 12. Sampling never moves the crossfade clocks ───────────────────────
    // Advance runs once per fixed step; sample runs once per RENDERED frame.
    // The clocks must be a function of ticks alone, or the blend a player sees
    // depends on their frame rate — BUG-0053's class, in the pose. Same switch,
    // same twelve ticks, sampled once per tick in one run and three times in
    // the other: the clocks must come out bit-identical.
    {
        auto run = [&](int samplesPerTick) {
            Skeleton s = makeSkeleton(4);
            AnimClip cA = makeClip(s, 2.0f), cB = makeClip(s, 3.0f);
            Fixture f;
            Animator a{}; a.playing = true; a.looping = true;
            a.fade = 0.5f; a.speed = 1.3f;
            f.build(s, &cA, a);
            const AnimClipHandle hB = f.clips.add(AnimClip(cB));
            f.sys.advance(1.0f / 60.0f); f.sys.sample();
            f.e.get_mut<Animator>().clip = hB;
            for (int t = 0; t < 12; ++t) {
                f.sys.advance(1.0f / 60.0f);
                for (int k = 0; k < samplesPerTick; ++k) f.sys.sample();
            }
            AnimatorSystem::FadeState fs;
            f.sys.fadeState(f.e.id(), fs);
            return fs;
        };
        const AnimatorSystem::FadeState one = run(1), three = run(3);
        CHECK(one.active && three.active,
              "both runs are mid-crossfade after 12 ticks (0.2 s of 0.5 s)");
        CHECK(one.elapsed == three.elapsed && one.prevTime == three.prevTime,
              "and the clocks are bit-identical at 1 and 3 samples per tick "
              "(elapsed %.6f vs %.6f, prevTime %.6f vs %.6f)",
              one.elapsed, three.elapsed, one.prevTime, three.prevTime);
        CHECK(one.prevTime > 0.0f,
              "and the outgoing clip kept advancing (%.4f) — a frozen clock "
              "would satisfy the line above", one.prevTime);
    }

    testwd::phase("13. A crossfade completes; fade 0 is a hard cut");

    // ── 13. A crossfade completes; fade 0 is a hard cut ─────────────────────
    {
        Skeleton s = makeSkeleton(4);
        AnimClip cA = makeClip(s, 2.0f), cB = makeClip(s, 3.0f);
        Fixture f;
        Animator a{}; a.playing = true; a.looping = true; a.fade = 0.2f;
        f.build(s, &cA, a);
        const AnimClipHandle hA = f.anim().clip;
        const AnimClipHandle hB = f.clips.add(AnimClip(cB));
        f.tick(0.05f);
        f.e.get_mut<Animator>().clip = hB;
        f.tick(0.1f);
        AnimatorSystem::FadeState fs;
        CHECK(f.sys.fadeState(f.e.id(), fs) && fs.active,
              "mid-crossfade after 0.1 s of 0.2 s");
        f.tick(0.15f);
        CHECK(f.sys.fadeState(f.e.id(), fs) && !fs.active,
              "and finished once 0.25 s have elapsed — the outgoing clip is "
              "released");
        f.e.get_mut<Animator>().fade = 0.0f;
        f.e.get_mut<Animator>().clip = hA;
        f.tick(0.05f);
        CHECK(f.sys.fadeState(f.e.id(), fs) && !fs.active,
              "with fade 0, a clip change is a HARD CUT — no crossfade starts");
    }

    testwd::phase("14. A destroyed entity's context is released");

    // ── 14. A destroyed entity's context is released ────────────────────────
    // m_contexts is keyed by entity id and was cleared only when the whole
    // world died (resetWorldCache). The SkinnedMesh release hook frees the
    // palette slot and nothing else, so every animated entity a session ever
    // spawned kept its ozz buffers — two sampling contexts and five vectors
    // sized to the skeleton — until Play stopped. A spawner is exactly the
    // content that does that thousands of times.
    {
        Skeleton s = makeSkeleton(4);
        AnimClip c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true;
        f.build(s, &c, a);
        f.tick(1.0f / 60.0f);
        const size_t base = f.sys.contextCount();

        std::vector<flecs::entity> spawned;
        for (int i = 0; i < 16; ++i) {
            SkinnedMesh sm{}; sm.skeleton = f.skeletons.add(Skeleton(s));
            Animator ai{}; ai.playing = true; ai.clip = f.anim().clip;
            spawned.push_back(f.ecs.entity().set<Animator>(ai).set<SkinnedMesh>(sm));
        }
        f.tick(1.0f / 60.0f);
        CHECK(f.sys.contextCount() == base + 16,
              "one context per live animated entity (%zu, want %zu)",
              f.sys.contextCount(), base + 16);

        for (flecs::entity e : spawned) e.destruct();
        f.tick(1.0f / 60.0f);
        CHECK(f.sys.contextCount() == base,
              "and destroying them releases their contexts (%zu, want %zu) — "
              "before this, they lived until Play stopped",
              f.sys.contextCount(), base);

        // Non-discriminating today, and labelled as such: a recycled id carries
        // a new generation, so it could not reach a dead entity's context even
        // before the fix. It guards against re-keying the map by bare index.
        SkinnedMesh sm{}; sm.skeleton = f.skeletons.add(Skeleton(s));
        Animator ai{}; ai.playing = true; ai.clip = f.anim().clip;
        flecs::entity reborn = f.ecs.entity().set<Animator>(ai).set<SkinnedMesh>(sm);
        f.tick(1.0f / 60.0f);
        AnimatorSystem::FadeState fs;
        CHECK(f.sys.fadeState(reborn.id(), fs) && !fs.active,
              "a newly spawned entity inherits no crossfade");
    }

    testwd::phase("15. Two worlds never share an animation context");

    // ── 15. Two worlds never share an animation context ─────────────────────
    // One AnimatorSystem animates the edit world through advance()/sample()
    // and a play-snapshot world through advance(world)/sample(world), and the
    // contexts used to live in ONE map keyed by entity id — so two worlds'
    // entities with equal ids shared a context, crossfade bookkeeping included.
    // The runtime happens to drive only one world per frame (runtime_sim.cpp's
    // `if (!m_gameWorld)` guard), which kept that latent. But the sweep
    // BUG-0061 needed would, over a shared map, erase every other world's
    // contexts on each pass — so the moment any host interleaved two worlds,
    // every crossfade would restart every pass. Interleaving here on purpose.
    {
        Skeleton s = makeSkeleton(4);
        AnimClip cA = makeClip(s, 2.0f), cB = makeClip(s, 3.0f);
        Fixture f;                              // the "edit" world
        Animator a{}; a.playing = true; a.looping = true; a.fade = 0.5f;
        f.build(s, &cA, a);
        const AnimClipHandle hA = f.anim().clip;
        const AnimClipHandle hB = f.clips.add(AnimClip(cB));

        flecs::world w2;                        // the "play" world
        w2.component<Animator>();
        w2.component<SkinnedMesh>();
        // BEFORE any entity carries SkinnedMesh. The first version of this
        // section skipped it, let the animator first see w2 after its entity
        // existed, and died in flecs (ALREADY_IN_USE) — which is how BUG-0062,
        // the same order in Snapshot Play, was found.
        f.sys.prepareWorld(w2);
        SkinnedMesh sm2{}; sm2.skeleton = f.skeletons.add(Skeleton(s));
        Animator a2 = a; a2.clip = hA;
        flecs::entity e2 = w2.entity().set<Animator>(a2).set<SkinnedMesh>(sm2);
        const bool sameId = e2.id() == f.e.id();
        std::printf("  info  edit-world entity %llu, play-world entity %llu — "
                    "ids %s\n", (unsigned long long)f.e.id(),
                    (unsigned long long)e2.id(),
                    sameId ? "EQUAL: this is the collision case" : "differ");

        const float dt = 1.0f / 60.0f;
        f.sys.advance(w2, dt); f.sys.advance(dt);     // both see clip A first
        e2.get_mut<Animator>().clip = hB;             // a fade in the PLAY world only
        for (int t = 0; t < 6; ++t) {
            f.sys.advance(w2, dt); f.sys.sample(w2);
            f.sys.advance(dt);     f.sys.sample();    // interleaved with the edit world
        }
        AnimatorSystem::FadeState play{}, edit{};
        CHECK(f.sys.fadeState(w2.c_ptr(), e2.id(), play) && play.active,
              "the play world's clip change started a crossfade");
        CHECK(std::fabs(play.elapsed - 6.0f * dt) < 1e-5f,
              "and it ACCUMULATED across interleaved passes over both worlds "
              "(%.4f, want %.4f) — a sweep over a shared map restarts it "
              "every pass", play.elapsed, 6.0f * dt);
        CHECK(f.sys.fadeState(f.e.id(), edit) && !edit.active,
              "while the edit world's entity%s has no crossfade — it never "
              "changed clip", sameId ? ", which has the SAME id," : "");
        f.sys.resetWorldCache();                     // the play world ends
        CHECK(f.sys.fadeState(f.e.id(), edit),
              "and ending the play world keeps the edit world's contexts");
    }

    testwd::phase("16. A world already in use is animated, not aborted");

    // ── 16. run()'s fallback must not abort (BUG-0062) ──────────────────────
    // prepareWorld() is the correct entry, and startSimulation uses it. A host
    // that never calls it reaches run()'s lazy install instead, on a world
    // whose SkinnedMesh may already be in use — where flecs cannot set a hook
    // and aborts. The fallback checks first and warns instead. This world is
    // built in exactly the order Snapshot Play used to die in.
    {
        Skeleton s = makeSkeleton(4);
        AnimClip c = makeClip(s, 1.0f);
        Fixture f;
        Animator a{}; a.playing = true;
        f.build(s, &c, a);
        flecs::world w3;
        SkinnedMesh sm3{}; sm3.skeleton = f.skeletons.add(Skeleton(s));
        Animator a3 = a; a3.clip = f.anim().clip;
        flecs::entity e3 = w3.entity().set<Animator>(a3).set<SkinnedMesh>(sm3);
        f.sys.advance(w3, 1.0f / 60.0f);       // aborted in flecs before the guard
        f.sys.sample(w3);
        CHECK(e3.get<Animator>().time > 0.0f,
              "a world whose SkinnedMesh was in use before the animator saw it "
              "is animated (t=%.4f) instead of aborting in flecs",
              e3.get<Animator>().time);
        f.sys.resetWorldCache();
    }

    jobs::shutdown();
    testwd::end();
    if (g_failures) {
        std::printf("animator_system_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("animator_system_test: PASS\n");
    return 0;
}
