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

    void tick(float dt) {
        if (!m_skeletons || !m_clips) return;

        m_query.each([&](flecs::entity, Animator& anim, SkinnedMesh& skin) {
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

            const int n = skel->boneCount();
            float worldMatrices[kMaxBones * 16];
            anim::computeWorldMatrices(*skel, localPose, worldMatrices);
            anim::computeSkinMatrices(*skel, worldMatrices, skin.skinMatrices);
            skin.hasSkinMatrices = true;
        });
    }

private:
    static constexpr int kMaxBones = 128;

    void computeBindPosePalette(const Skeleton& skel, SkinnedMesh& skin) {
        if (skel.boneCount() > kMaxBones) { skin.hasSkinMatrices = false; return; }
        Pose bind = anim::bindPose(skel);
        const int n = skel.boneCount();
        float worldMatrices[kMaxBones * 16];
        anim::computeWorldMatrices(skel, bind, worldMatrices);
        anim::computeSkinMatrices(skel, worldMatrices, skin.skinMatrices);
        skin.hasSkinMatrices = true;
    }

    SkeletonRegistry* m_skeletons = nullptr;
    AnimClipRegistry* m_clips     = nullptr;
    flecs::query<Animator, SkinnedMesh> m_query;
};
