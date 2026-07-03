#pragma once
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

class AnimatorSystem {
public:
    void init(flecs::world& ecs,
              SkeletonRegistry& skeletons,
              AnimClipRegistry& clips) {
        m_skeletons = &skeletons;
        m_clips     = &clips;

        m_query = ecs.query_builder<Animator, SkinnedMesh>().build();
    }

    // Tick the world the system was init()ed with (cached query).
    void tick(float dt) {
        if (!m_skeletons || !m_clips) return;
        collect(m_query);
        stepAll(dt);
    }

    // Tick an arbitrary world — the play-mode snapshot world. The query is
    // cached per world; the runtime calls resetWorldCache() when the
    // snapshot world is destroyed (sim stop).
    void tick(flecs::world& world, float dt) {
        if (!m_skeletons || !m_clips) return;
        collect(m_worldQuery.get(world));
        stepAll(dt);
    }

    void resetWorldCache() {
        m_worldQuery.reset();
        m_contexts.clear();   // sim-world entity ids die with the world
    }

private:
    static constexpr int kMaxBones2 = kMaxBones;   // palette budget (skeleton.h)

    // Per-entity ozz runtime buffers. Sized to the skeleton on first use;
    // resized if the entity's skeleton changes. Crossfade state lives here
    // (not on the component — components get snapshot-copied at Play):
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

    // SERIAL: query iteration + m_contexts inserts (rehash) live here, so the
    // parallel phase touches the map read-only through stable pointers.
    template <typename Query>
    void collect(Query&& q) {
        m_work.clear();
        q.each([&](flecs::entity e, Animator& anim, SkinnedMesh& skin) {
            m_work.push_back({&anim, &skin, &m_contexts[e.id()]});
        });
    }

    // PARALLEL: entities are independent (own context, own components, own
    // crossfade state; registries are read-only during the tick), so this is
    // a flat parallelFor. Grain 1 — one entity's sampling is real work.
    void stepAll(float dt) {
        jobs::parallelFor("anim.sample", (uint32_t)m_work.size(), 1,
            [&](uint32_t begin, uint32_t end) {
                for (uint32_t i = begin; i < end; ++i)
                    step(*m_work[i].anim, *m_work[i].skin, *m_work[i].ctx, dt);
            });
    }

    // Advance time, sample the clip, write the bone palette for one entity.
    void step(Animator& anim, SkinnedMesh& skin, AnimContext& ctx, float dt) {
        const Skeleton* skel = m_skeletons->get(skin.skeleton);
        if (!skel || skel->boneCount() == 0 || !skel->ozz) {
            skin.hasSkinMatrices = false;
            return;
        }
        if (skel->boneCount() > kMaxBones2) {
            skin.hasSkinMatrices = false;
            return;
        }

        const AnimClip* clip = m_clips->get(anim.clip);

        // No clip: render bind pose (raw-matrix path — precision invariant).
        if (!clip || !clip->valid()) {
            computeBindPosePalette(*skel, skin);
            return;
        }

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
            ctx.fadeElapsed += dt;
            const float alpha = ctx.fadeDuration > 0.0f
                ? std::min(ctx.fadeElapsed / ctx.fadeDuration, 1.0f) : 1.0f;
            if (alpha >= 1.0f) {
                ctx.prevClip = {};   // fade complete
            } else {
                // Outgoing clip keeps advancing (looped) so it doesn't freeze.
                ctx.prevTime += dt * anim.speed;
                if (prev->duration > 0.0f) {
                    ctx.prevTime = std::fmod(ctx.prevTime, prev->duration);
                    if (ctx.prevTime < 0.0f) ctx.prevTime += prev->duration;
                }
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
        for (int i = 0; i < skel->boneCount(); ++i) {
            float model[16];
            const ozz::math::Float4x4& m = ctx.models[(size_t)skel->ozzJointOf[i]];
            for (int c = 0; c < 4; ++c)
                ozz::math::StorePtrU(m.cols[c], &model[c * 4]);
            bx::mtxMul(&skin.skinMatrices[i * 16],
                       skel->bones[i].inverseBindMatrix, model);
        }
        skin.hasSkinMatrices = true;
    }

    void computeBindPosePalette(const Skeleton& skel, SkinnedMesh& skin) {
        if (skel.boneCount() > kMaxBones2) { skin.hasSkinMatrices = false; return; }
        float worldMatrices[kMaxBones * 16];
        anim::computeBindPoseWorldMatrices(skel, worldMatrices);
        anim::computeSkinMatrices(skel, worldMatrices, skin.skinMatrices);
        skin.hasSkinMatrices = true;
    }

    SkeletonRegistry* m_skeletons = nullptr;
    AnimClipRegistry* m_clips     = nullptr;
    flecs::query<Animator, SkinnedMesh> m_query;       // init() world
    WorldQueryCache<Animator, SkinnedMesh> m_worldQuery; // sim/snapshot world
    std::unordered_map<uint64_t, AnimContext> m_contexts; // per-entity ozz state
    std::vector<WorkItem> m_work;   // this frame's entities (collect -> stepAll)
};
