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
        m_query.each([&](flecs::entity e, Animator& anim, SkinnedMesh& skin) {
            step(e.id(), anim, skin, dt);
        });
    }

    // Tick an arbitrary world — the play-mode snapshot world. The query is
    // cached per world; the runtime calls resetWorldCache() when the
    // snapshot world is destroyed (sim stop).
    void tick(flecs::world& world, float dt) {
        if (!m_skeletons || !m_clips) return;
        m_worldQuery.get(world)
            .each([&](flecs::entity e, Animator& anim, SkinnedMesh& skin) {
                step(e.id(), anim, skin, dt);
            });
    }

    void resetWorldCache() {
        m_worldQuery.reset();
        m_contexts.clear();   // sim-world entity ids die with the world
    }

private:
    static constexpr int kMaxBones2 = kMaxBones;   // palette budget (skeleton.h)

    // Per-entity ozz runtime buffers. Sized to the skeleton on first use;
    // resized if the entity's skeleton changes.
    struct AnimContext {
        ozz::animation::SamplingJob::Context     sampling;
        std::vector<ozz::math::SoaTransform>     locals;
        std::vector<ozz::math::Float4x4>         models;
        const ozz::animation::Skeleton*          builtFor = nullptr;

        void ensure(const ozz::animation::Skeleton& skel) {
            if (builtFor == &skel) return;
            sampling.Resize(skel.num_joints());
            locals.resize((size_t)skel.num_soa_joints());
            models.resize((size_t)skel.num_joints());
            builtFor = &skel;
        }
    };

    // Advance time, sample the clip, write the bone palette for one entity.
    void step(uint64_t entityId, Animator& anim, SkinnedMesh& skin, float dt) {
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

        AnimContext& ctx = m_contexts[entityId];
        ctx.ensure(*skel->ozz);

        // ── ozz: compressed clip -> SoA locals -> model-space matrices ──────
        ozz::animation::SamplingJob sample;
        sample.animation = clip->ozz.get();
        sample.context   = &ctx.sampling;
        sample.ratio     = clip->duration > 0.0f ? anim.time / clip->duration : 0.0f;
        sample.output    = ozz::make_span(ctx.locals);
        if (!sample.Run()) { skin.hasSkinMatrices = false; return; }

        ozz::animation::LocalToModelJob l2m;
        l2m.skeleton = skel->ozz.get();
        l2m.input    = ozz::make_span(ctx.locals);
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
};
