#pragma once
// AnimatorSystem — runs every frame, advances animation time, samples clips,
// computes per-entity bone palettes. Writes into SkinnedMesh::skinMatrices
// which the renderer reads at extraction time.
//
// Query: entities with Animator + SkinnedMesh (MeshRenderer optional — an
// entity can have animation data even if its mesh hasn't loaded yet).

#include <cmath>
#include <cstring>
#include <flecs.h>

#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "animation/pose.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include "components/animator.h"
#include "components/skinned_mesh.h"

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
        m_query.each([&](flecs::entity, Animator& anim, SkinnedMesh& skin) {
            step(anim, skin, dt);
        });
    }

    // Tick an arbitrary world — the play-mode snapshot world. Builds a
    // transient query (snapshot worlds are short-lived; not worth caching).
    void tick(flecs::world& world, float dt) {
        if (!m_skeletons || !m_clips) return;
        world.query_builder<Animator, SkinnedMesh>().build()
            .each([&](flecs::entity, Animator& anim, SkinnedMesh& skin) {
                step(anim, skin, dt);
            });
    }

private:
    static constexpr int kMaxBones = 128;

    // Advance time, sample the clip, write the bone palette for one entity.
    void step(Animator& anim, SkinnedMesh& skin, float dt) {
        // Skip if no skeleton or clip
        const Skeleton* skel = m_skeletons->get(skin.skeleton);
        if (!skel || skel->boneCount() == 0) {
            skin.hasSkinMatrices = false;
            return;
        }
        // Guard: bone count must fit within the fixed-size palette buffers.
        if (skel->boneCount() > kMaxBones) {
            skin.hasSkinMatrices = false;
            return;
        }

        const AnimClip* clip = m_clips->get(anim.clip);

        // No clip: render bind pose
        if (!clip || clip->channels.empty()) {
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
                // Clamp to [0, duration]
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

        // Sample clip -> local pose -> world matrices -> skin matrices
        Pose localPose = anim::sampleClip(*clip, *skel, anim.time);

        float worldMatrices[kMaxBones * 16];
        anim::computeWorldMatrices(*skel, localPose, worldMatrices);
        anim::computeSkinMatrices(*skel, worldMatrices, skin.skinMatrices);
        skin.hasSkinMatrices = true;
    }

    void computeBindPosePalette(const Skeleton& skel, SkinnedMesh& skin) {
        if (skel.boneCount() > kMaxBones) { skin.hasSkinMatrices = false; return; }
        float worldMatrices[kMaxBones * 16];
        anim::computeBindPoseWorldMatrices(skel, worldMatrices);
        anim::computeSkinMatrices(skel, worldMatrices, skin.skinMatrices);
        skin.hasSkinMatrices = true;
    }

    SkeletonRegistry* m_skeletons = nullptr;
    AnimClipRegistry* m_clips     = nullptr;
    flecs::query<Animator, SkinnedMesh> m_query;
};
