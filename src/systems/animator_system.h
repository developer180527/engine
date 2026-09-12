#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "animation/skin_palette.h"   // palettes live out of the component
// AnimatorSystem — runs every frame, advances animation time, samples clips,
// computes per-entity bone palettes. Writes into SkinnedMesh::skinMatrices
// which the renderer reads at extraction time.
//
// The sampling core is ozz-animation: SamplingJob (compressed clip -> SoA
// local transforms) then LocalToModelJob (-> model-space joint matrices),
// remapped from ozz joint order to OUR bone order for the IBM multiply. ozz
// Float4x4 column-major memory is byte-identical to bx row-major row-vector
// memory, so model matrices store straight into the bx pipeline (see
// animation/ozz_bridge.h + animation/info.md for the convention story).
//
// Per-entity ozz runtime state (sampling context + SoA/model buffers) can't
// live in the ECS — components get snapshot-copied at Play — so it's owned
// here, keyed by entity id, and dropped with the world cache.
//
// Query: entities with Animator + SkinnedMesh (MeshRenderer optional — an
// entity can have animation data even if its mesh hasn't loaded yet).

#include <cmath>
#include <cstring>
#include <flecs.h>
#include <memory>
#include <unordered_map>

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/span.h>

#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "animation/pose.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"
#include "runtime/jobs/jobs.h"
#include "runtime/world_query_cache.h"
#include <algorithm>

class AnimatorSystem {
public:
    // Which half of a tick's work to do. Declared here because run() names it
    // in its signature, and a member type must exist before that point.
    enum class Phase { Advance, Sample };

    void init(flecs::world& ecs,
              SkeletonRegistry& skeletons,
              AnimClipRegistry& clips) {
        m_skeletons = &skeletons;
        m_clips     = &clips;

        m_query = ecs.query_builder<Animator, SkinnedMesh>().build();
        m_initWorld = ecs.c_ptr();
        installReleaseHook(ecs);
    }

    // ── ADVANCE and SAMPLE are separate, and that separation is the point ───
    // `Animator::time` is SIMULATION STATE: a hashed component
    // (components/sim_state.h), so it must advance exactly once per fixed step
    // at kSimDt. Sampling — ozz, blending, the bone palette — is PRESENTATION:
    // it reads the already-advanced time and belongs on the frame.
    //
    // The crossfade clocks (AnimContext::fadeElapsed, prevTime) also advance
    // here at kSimDt, but they are NOT simulation state — and this comment used
    // to say they were hashed components. They are not components at all, and
    // nothing in the simulation reads them; only sampleOne does, to weight the
    // blend. They advance per tick so the pose a player SEES does not depend on
    // their frame rate, which is a presentation property. By the gate's own
    // rule — hash what can influence future simulation — they stay unhashed. A
    // 2026-09-12 audit took this comment at its word and briefly called them a
    // blind spot in the gate; animator_system_test §11–13 now pin them instead.
    //
    // Before this split both happened in one tick() called at FRAME rate, so
    // `Animator::time` advanced at render rate. tests/determinism_gate_test.cpp
    // measures it directly: its `animator` tier builds skinned entities through
    // ozz's offline builders, and with this split reverted that tier's A/B
    // comparison diverges on the first tick.
    //
    // Worth recording how that claim was earned, because for a day it was not:
    // the gate's first build had no animator tier at all. It found the SPINNER
    // — the same defect two lines away in tickSystems — and this comment
    // asserted measurement for the animator on the strength of proximity. The
    // tier exists now, so the claim is rung 1 rather than rung 6.
    //
    // Splitting them also stops a hitch multiplying the expensive half — the
    // accumulator can run up to 4 fixed steps in one frame, and sampling four
    // poses when only the last is ever seen is pure waste.
    //
    // tick() keeps both, for the EDITOR preview: with no simulation running
    // there is no fixed step to hang the clocks on, and animation is expected
    // to play at frame rate in the viewport.
    void tick(float dt) { advance(dt); sample(); }
    void advance(float dt) {
        if (!m_skeletons || !m_clips) return;
        collect(m_query, m_initWorld);
        stepAll(dt, Phase::Advance);
    }
    void sample() {
        if (!m_skeletons || !m_clips) return;
        collect(m_query, m_initWorld);
        stepAll(0.0f, Phase::Sample);
    }

    // Tick an arbitrary world — the play-mode snapshot world. The query is
    // cached per world; the runtime calls resetWorldCache() when the
    // snapshot world is destroyed (sim stop).
    void tick(flecs::world& world, float dt) {
        advance(world, dt);
        sample(world);
    }
    void advance(flecs::world& world, float dt) { run(world, dt, Phase::Advance); }
    void sample (flecs::world& world)           { run(world, 0.0f, Phase::Sample); }

private:
    void run(flecs::world& world, float dt, Phase phase) {
        if (!m_skeletons || !m_clips) return;
        // ── The hook is PER WORLD, and this world is not the one init() saw ──
        // Component hooks are world state, so registering on the editor world in
        // init() did nothing for the play snapshot world — which is precisely
        // where entities are created, animated, and destroyed. Every play→stop
        // cycle leaked one 8 KB palette per animated entity, for the life of the
        // process. The old comment called that "bounded"; it is bounded per
        // session and unbounded across them.
        if (m_hookedWorld != world.c_ptr()) {
            // The FALLBACK for a host that never called prepareWorld(). It may
            // not simply set the hook: on a world whose SkinnedMesh is already
            // in use, flecs asserts and aborts (BUG-0062). So it checks first,
            // and on a world it is too late for it warns instead — a loud leak
            // (palette slots of that world's entities are not returned on
            // removal) rather than a crash.
            if (!ecs_id_in_use(world.c_ptr(), world.component<SkinnedMesh>().id())) {
                installReleaseHook(world);
            } else if (!m_warnedLateHook) {
                m_warnedLateHook = true;
                std::fprintf(stderr,
                    "[Animator] WARNING: this world already had SkinnedMesh "
                    "entities when the animator first saw it, so the palette "
                    "release hook cannot be installed and their palette slots "
                    "will not be returned on removal. Call "
                    "AnimatorSystem::prepareWorld() before filling the world.\n");
            }
            m_hookedWorld = world.c_ptr();
        }
        collect(m_worldQuery.get(world), world.c_ptr());
        stepAll(dt, phase);
    }

public:
    // ── Install this system's per-world hook on a world BEFORE it is filled ──
    // Component hooks are world state, and flecs refuses to set a hook on a
    // component that is already in use — it ASSERTS and the process aborts. So
    // a host creating a world for this system to animate must call this after
    // registering schemas and BEFORE any entity carries SkinnedMesh.
    // startSimulation does, for the Snapshot play world (BUG-0062: it used to
    // be left to run()'s lazy install, which reached a world the snapshot had
    // already filled, and Snapshot Play of any scene with a skinned entity
    // aborted on its first tick).
    void prepareWorld(flecs::world& world) {
        if (m_hookedWorld == world.c_ptr()) return;
        installReleaseHook(world);
        m_hookedWorld = world.c_ptr();
    }

    void resetWorldCache() {
        m_worldQuery.reset();
        // The sim world's contexts die with it — ONLY that world's, since they
        // are kept per world (m_worlds) and the edit world's entities are still
        // alive. Erased before m_hookedWorld is forgotten below and before the
        // world is freed: a new snapshot world can land at the SAME ADDRESS,
        // and a surviving entry keyed by that address would hand its entities a
        // dead world's crossfade state.
        if (m_hookedWorld) m_worlds.erase(m_hookedWorld);
        // Forget which world carries the hook. A new snapshot world can land on
        // the SAME ADDRESS as the dead one (the note at the call site says so),
        // and a stale match here would skip registration and resume leaking.
        m_hookedWorld = nullptr;
    }

    // ── Diagnostics ─────────────────────────────────────────────────────────
    // The crossfade state lives in m_contexts, outside every component, so a
    // test cannot observe it any other way — the same reason JoltPlugin has
    // characterRotation(). Read-only; the system itself never calls these.
    // Across every world this system has animated.
    size_t contextCount() const {
        size_t n = 0;
        for (const auto& [world, wc] : m_worlds) n += wc.map.size();
        return n;
    }
    struct FadeState {
        bool  active   = false;
        float elapsed  = 0.0f;
        float duration = 0.0f;
        float prevTime = 0.0f;
    };
    // In the world init() was given, or in a named one.
    bool fadeState(flecs::entity_t e, FadeState& out) const {
        return fadeState(m_initWorld, e, out);
    }
    bool fadeState(const ecs_world_t* world, flecs::entity_t e,
                   FadeState& out) const {
        auto w = m_worlds.find(world);
        if (w == m_worlds.end()) return false;
        auto it = w->second.map.find(e);
        if (it == w->second.map.end()) return false;
        out.active   = it->second.prevClip.valid();
        out.elapsed  = it->second.fadeElapsed;
        out.duration = it->second.fadeDuration;
        out.prevTime = it->second.prevTime;
        return true;
    }

private:
    // Give the slot back when the component goes away — on entity destruction,
    // on an explicit remove, and on world teardown. Without it a destroyed
    // entity leaks 8 KB, which a horde shooter does thousands of times a match.
    static void installReleaseHook(flecs::world& w) {
        w.component<SkinnedMesh>().on_remove(
            [](flecs::entity, SkinnedMesh& s) {
                anim::skinPalettes().release(s.paletteSlot);
                s.paletteSlot = SkinnedMesh::kNoSlot;
            });
    }

    static constexpr int kMaxBones2 = kMaxBones;   // palette budget (skeleton.h)

    // Per-entity ozz runtime buffers. Sized to the skeleton on first use;
    // resized if the entity's skeleton changes. Crossfade state lives here,
    // not on the component, for two reasons: components are snapshot-copied at
    // Play, and — the one that is not negotiable — Animator is part of the kit
    // ABI. It is hashed into engine_abi::componentLayoutHash
    // (include/engine/game_module.h) and its field order is pinned by
    // component_abi_test, so a field added to it makes the module loader
    // refuse every kit and the game module until they are rebuilt.
    // How it works:
    // when the Animator's clip handle CHANGES, the old clip keeps playing and
    // fades out over Animator::fade seconds via ozz BlendingJob.
    struct AnimContext {
        // unique_ptr: SamplingJob::Context is not movable/swappable; the
        // crossfade swap exchanges pointers instead.
        std::unique_ptr<ozz::animation::SamplingJob::Context> sampling;     // current
        std::unique_ptr<ozz::animation::SamplingJob::Context> samplingPrev; // fading out
        std::vector<ozz::math::SoaTransform>     locals;
        std::vector<ozz::math::SoaTransform>     localsPrev;
        std::vector<ozz::math::SoaTransform>     localsBlend;
        std::vector<ozz::math::Float4x4>         models;
        const ozz::animation::Skeleton*          builtFor = nullptr;

        // Transition bookkeeping
        AnimClipHandle lastClip{};      // clip seen last frame (switch detection)
        float          lastTime = 0.0f;
        AnimClipHandle prevClip{};      // the clip being faded OUT (invalid = none)
        float          prevTime = 0.0f;
        float          fadeElapsed = 0.0f;
        float          fadeDuration = 0.0f;
        uint64_t       seenEpoch    = 0;   // last collect() that saw it (BUG-0061)

        // Fails loudly with the buffer's name and address. A misaligned buffer
        // here is always a bug, never a tolerable condition, so this reports
        // rather than degrades.
        static void assertSimdAligned(const char* what, const void* p) {
            if (!p) return;   // an empty vector may legitimately hold nullptr
            const auto addr = reinterpret_cast<std::uintptr_t>(p);
            if ((addr & 0xF) == 0) return;
            std::fprintf(stderr,
                "[Animator] FATAL: %s is at %p, which is %u-byte aligned, not 16.\n"
                "  ozz writes this buffer with aligned SIMD stores; on x86 that\n"
                "  faults. The allocator behind std::vector did not honour the\n"
                "  type's alignas(16) — check the aligned operator new overloads\n"
                "  in src/core/mem_counters.cpp and mem::alloc's align handling.\n",
                what, p, (unsigned)(addr & (~addr + 1)));
            std::abort();
        }

        void ensure(const ozz::animation::Skeleton& skel) {
            if (builtFor == &skel) return;
            if (!sampling)     sampling     = std::make_unique<ozz::animation::SamplingJob::Context>();
            if (!samplingPrev) samplingPrev = std::make_unique<ozz::animation::SamplingJob::Context>();
            sampling->Resize(skel.num_joints());
            samplingPrev->Resize(skel.num_joints());
            locals.resize((size_t)skel.num_soa_joints());
            localsPrev.resize((size_t)skel.num_soa_joints());
            localsBlend.resize((size_t)skel.num_soa_joints());
            models.resize((size_t)skel.num_joints());

            // ── The buffers ozz writes with ALIGNED SIMD stores ─────────────
            // SoaTransform and Float4x4 are alignas(16), so the allocator must
            // honour that: ozz uses _mm_store_ps on x86, which FAULTS on a
            // misaligned address, while NEON tolerates it. Get this wrong and
            // the failure is a bare SEGFAULT with no stack, on one architecture
            // only — which is exactly what Windows x64 reports today while
            // Linux x64, macOS arm64 and Windows arm64 all pass.
            //
            // Checked rather than assumed, because the default allocation
            // alignment is NOT the same everywhere: alignof(max_align_t) is 16
            // on Linux/macOS x86-64 and 8 on MSVC. A buffer that reaches ozz at
            // 8-byte alignment is fine on three of our four platforms.
            //
            // Diagnostic, not a guess at the cause: it names the buffer and the
            // address instead of dying silently, so one CI run answers the
            // question rather than narrowing it.
            assertSimdAligned("locals",      locals.data());
            assertSimdAligned("localsPrev",  localsPrev.data());
            assertSimdAligned("localsBlend", localsBlend.data());
            assertSimdAligned("models",      models.data());
            builtFor = &skel;
        }
    };

    // One frame's work items. Component pointers stay valid between collect
    // and stepAll: both happen inside one tick, with no structural ECS
    // changes in between. Context pointers survive map growth — unordered_map
    // rehash moves buckets, not nodes.
    struct WorkItem {
        Animator*    anim;
        SkinnedMesh* skin;
        AnimContext* ctx;
    };

    // SERIAL: query iteration + context inserts (rehash) live here, so the
    // parallel phase touches the map read-only through stable pointers.
    //
    // ── Released when their entity is gone (BUG-0061) ───────────────────────
    // Contexts used to be cleared only when a whole world died, so every
    // animated entity a session ever spawned kept its ozz buffers until Play
    // stopped. Each pass now stamps the contexts it collects; when the world's
    // map holds MORE contexts than the pass collected — the only case in which
    // anything can be stale — the unstamped ones are erased. The steady state
    // pays one integer store per entity and never sweeps.
    //
    // Why a sweep and not an on_remove hook: SkinnedMesh already carries the
    // one on_remove hook flecs allows per component type (the palette release
    // below), and a hook on Animator — a component kits share — would collide
    // with any kit that sets its own.
    //
    // Why PER WORLD: the sweep is only correct over entities this pass could
    // have seen. With one map for every world, a pass over one world would
    // erase every other world's contexts, and interleaved passes would restart
    // every crossfade on every pass. The runtime drives one world per frame
    // today; this class accepts both, so it must not depend on that.
    //
    // Erasing from an unordered_map invalidates only the erased nodes, so the
    // pointers m_work holds into live contexts stay valid for stepAll.
    template <typename Query>
    void collect(Query&& q, const ecs_world_t* world) {
        m_work.clear();
        WorldContexts& wc = m_worlds[world];
        ++wc.epoch;
        q.each([&](flecs::entity e, Animator& anim, SkinnedMesh& skin) {
            AnimContext& ctx = wc.map[e.id()];
            ctx.seenEpoch = wc.epoch;
            m_work.push_back({&anim, &skin, &ctx});
        });
        if (wc.map.size() > m_work.size()) {
            for (auto it = wc.map.begin(); it != wc.map.end();) {
                if (it->second.seenEpoch != wc.epoch) it = wc.map.erase(it);
                else ++it;
            }
        }
    }

    // PARALLEL: entities are independent (own context, own components, own
    // crossfade state; registries are read-only during the tick), so this is
    // a flat parallelFor. Grain 1 — one entity's sampling is real work.
    void stepAll(float dt, Phase phase) {
        jobs::parallelFor(phase == Phase::Advance ? "anim.advance" : "anim.sample",
            (uint32_t)m_work.size(), 1,
            [&](uint32_t begin, uint32_t end) {
                for (uint32_t i = begin; i < end; ++i)
                    step(*m_work[i].anim, *m_work[i].skin, *m_work[i].ctx,
                         dt, phase);
            });
    }

    // Advance time, sample the clip, write the bone palette for one entity.
    void step(Animator& anim, SkinnedMesh& skin, AnimContext& ctx, float dt,
              Phase phase) {
        // hasSkinMatrices and the palette are PRESENTATION, so the advance
        // phase leaves them alone. An entity with a broken skeleton keeps
        // exactly the behaviour it had before the split, including not
        // advancing its clock.
        const Skeleton* skel = m_skeletons->get(skin.skeleton);
        if (!skel || skel->boneCount() == 0 || !skel->ozz) {
            if (phase == Phase::Sample) skin.hasSkinMatrices = false;
            return;
        }
        if (skel->boneCount() > kMaxBones2) {
            if (phase == Phase::Sample) skin.hasSkinMatrices = false;
            return;
        }

        const AnimClip* clip = m_clips->get(anim.clip);

        // No clip: render bind pose (raw-matrix path — precision invariant).
        if (!clip || !clip->valid()) {
            if (phase == Phase::Sample) computeBindPosePalette(*skel, skin);
            return;
        }

        if (phase == Phase::Sample) {
            sampleOne(anim, skin, ctx, *skel, *clip);
            return;
        }

        // ══ ADVANCE ═══════════════════════════════════════════════════════
        // Everything below mutates simulation state and nothing else:
        // Animator::time, Animator::playing, and the crossfade clocks.

        // Advance time
        if (anim.playing) {
            anim.time += dt * anim.speed;

            if (anim.looping && clip->duration > 0.0f) {
                anim.time = std::fmod(anim.time, clip->duration);
                if (anim.time < 0.0f) anim.time += clip->duration;
            } else {
                if (anim.time > clip->duration) {
                    anim.time    = clip->duration;
                    anim.playing = false;
                }
                if (anim.time < 0.0f) {
                    anim.time    = 0.0f;
                    anim.playing = false;
                }
            }
        }

        ctx.ensure(*skel->ozz);

        // ── Crossfade bookkeeping: clip handle changed -> fade the old out ──
        if (ctx.lastClip.valid() && anim.clip.id != ctx.lastClip.id) {
            if (anim.fade > 0.0f && m_clips->get(ctx.lastClip)) {
                ctx.prevClip     = ctx.lastClip;
                ctx.prevTime     = ctx.lastTime;
                ctx.fadeElapsed  = 0.0f;
                ctx.fadeDuration = anim.fade;
                // The warm sampling context belongs to the OLD clip now.
                std::swap(ctx.sampling, ctx.samplingPrev);
            } else {
                ctx.prevClip = {};   // hard cut
            }
        }
        ctx.lastClip = anim.clip;
        ctx.lastTime = anim.time;

        // ── Fade progress ──────────────────────────────────────────────────
        // Hoisted out of the sampling block, where it used to sit between the
        // two SamplingJobs. These are CLOCKS, so they belong in this phase; the
        // sampler now recomputes alpha from them instead of advancing them.
        if (ctx.prevClip.valid()) {
            const AnimClip* prevClip = m_clips->get(ctx.prevClip);
            if (!prevClip || !prevClip->valid()) {
                ctx.prevClip = {};
            } else {
                ctx.fadeElapsed += dt;
                const float alpha = ctx.fadeDuration > 0.0f
                    ? std::min(ctx.fadeElapsed / ctx.fadeDuration, 1.0f) : 1.0f;
                if (alpha >= 1.0f) {
                    ctx.prevClip = {};                 // fade complete
                } else {
                    // The outgoing clip keeps advancing (looped) so it does not
                    // freeze mid-blend.
                    ctx.prevTime += dt * anim.speed;
                    if (prevClip->duration > 0.0f) {
                        ctx.prevTime = std::fmod(ctx.prevTime, prevClip->duration);
                        if (ctx.prevTime < 0.0f) ctx.prevTime += prevClip->duration;
                    }
                }
            }
        }
    }

    // ══ SAMPLE ═════════════════════════════════════════════════════════════
    // Pure presentation: reads the clocks Phase::Advance moved and writes the
    // bone palette. It mutates no component the determinism gate hashes, which
    // is precisely why it is allowed to keep running at frame rate.
    void sampleOne(const Animator& anim, SkinnedMesh& skin, AnimContext& ctx,
                   const Skeleton& skelRef, const AnimClip& clipRef) {
        const Skeleton* skel = &skelRef;
        const AnimClip* clip = &clipRef;
        ctx.ensure(*skel->ozz);

        // ── ozz: compressed clip -> SoA locals -> model-space matrices ──────
        ozz::animation::SamplingJob sample;
        sample.animation = clip->ozz.get();
        sample.context   = ctx.sampling.get();
        sample.ratio     = clip->duration > 0.0f ? anim.time / clip->duration : 0.0f;
        sample.output    = ozz::make_span(ctx.locals);
        if (!sample.Run()) { skin.hasSkinMatrices = false; return; }

        // Fading? Sample the outgoing clip too and blend by fade progress.
        const ozz::math::SoaTransform* finalLocals = ctx.locals.data();
        const AnimClip* prev = ctx.prevClip.valid() ? m_clips->get(ctx.prevClip) : nullptr;
        if (prev && prev->valid()) {
            // READ ONLY. fadeElapsed and prevTime were advanced in
            // Phase::Advance; touching them here would double-step the fade
            // whenever the frame rate and the tick rate differ.
            const float alpha = ctx.fadeDuration > 0.0f
                ? std::min(ctx.fadeElapsed / ctx.fadeDuration, 1.0f) : 1.0f;
            if (alpha < 1.0f) {
                ozz::animation::SamplingJob samplePrev;
                samplePrev.animation = prev->ozz.get();
                samplePrev.context   = ctx.samplingPrev.get();
                samplePrev.ratio     = prev->duration > 0.0f
                                     ? ctx.prevTime / prev->duration : 0.0f;
                samplePrev.output    = ozz::make_span(ctx.localsPrev);
                if (samplePrev.Run()) {
                    ozz::animation::BlendingJob::Layer layers[2];
                    layers[0].transform = ozz::make_span(ctx.localsPrev);
                    layers[0].weight    = 1.0f - alpha;
                    layers[1].transform = ozz::make_span(ctx.locals);
                    layers[1].weight    = alpha;

                    ozz::animation::BlendingJob blend;
                    blend.layers    = layers;
                    blend.rest_pose = skel->ozz->joint_rest_poses();
                    blend.output    = ozz::make_span(ctx.localsBlend);
                    if (blend.Run()) finalLocals = ctx.localsBlend.data();
                }
            }
        }

        ozz::animation::LocalToModelJob l2m;
        l2m.skeleton = skel->ozz.get();
        l2m.input    = ozz::span<const ozz::math::SoaTransform>(
                           finalLocals, ctx.locals.size());
        l2m.output   = ozz::make_span(ctx.models);
        if (!l2m.Run()) { skin.hasSkinMatrices = false; return; }

        // ── Palette: skin[our i] = IBM[our i] * model[ozz joint of i] ───────
        // ozz Float4x4 stores columns; its memory equals the bx row-vector
        // layout for the same transform — store unaligned, then bx::mtxMul.
        float* palette = paletteFor(skin);
        if (!palette) { skin.hasSkinMatrices = false; return; }
        for (int i = 0; i < skel->boneCount(); ++i) {
            float model[16];
            const ozz::math::Float4x4& m = ctx.models[(size_t)skel->ozzJointOf[i]];
            for (int c = 0; c < 4; ++c)
                ozz::math::StorePtrU(m.cols[c], &model[c * 4]);
            bx::mtxMul(&palette[i * 16],
                       skel->bones[i].inverseBindMatrix, model);
        }
        skin.hasSkinMatrices = true;
    }

    // Lazily takes a pool slot the first time this entity is animated, so an
    // entity that never animates costs a slot's worth of nothing.
    static float* paletteFor(SkinnedMesh& skin) {
        if (skin.paletteSlot == SkinnedMesh::kNoSlot)
            skin.paletteSlot = anim::skinPalettes().acquire();
        return anim::skinPalettes().at(skin.paletteSlot);
    }

    void computeBindPosePalette(const Skeleton& skel, SkinnedMesh& skin) {
        if (skel.boneCount() > kMaxBones2) { skin.hasSkinMatrices = false; return; }
        float worldMatrices[kMaxBones * 16];
        anim::computeBindPoseWorldMatrices(skel, worldMatrices);
        float* palette = paletteFor(skin);
        if (!palette) { skin.hasSkinMatrices = false; return; }
        anim::computeSkinMatrices(skel, worldMatrices, palette);
        skin.hasSkinMatrices = true;
    }

    SkeletonRegistry* m_skeletons = nullptr;
    AnimClipRegistry* m_clips     = nullptr;
    flecs::query<Animator, SkinnedMesh> m_query;       // init() world
    WorldQueryCache<Animator, SkinnedMesh> m_worldQuery; // sim/snapshot world
    // Per WORLD, then per entity. One map keyed by entity id alone put two
    // worlds' entities in the same slot whenever their ids matched — and a
    // Snapshot-play game world is filled from the editor's scene, which is
    // exactly where matching ids are likely.
    struct WorldContexts {
        std::unordered_map<uint64_t, AnimContext> map;   // per-entity ozz state
        uint64_t epoch = 0;          // bumped per collect(); see there
    };
    std::unordered_map<const ecs_world_t*, WorldContexts> m_worlds;
    const ecs_world_t* m_initWorld = nullptr;   // the world init() was given
    std::vector<WorkItem> m_work;   // this frame's entities (collect -> stepAll)
    // Which world currently carries the palette-release hook. Only ever the most
    // recent snapshot world: the editor world's registration happens in init()
    // and, being world state, never needs renewing.
    const ecs_world_t* m_hookedWorld = nullptr;
    bool               m_warnedLateHook = false;   // run()'s fallback, once
};
