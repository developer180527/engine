#pragma once

#include "core/handle.h"

// SkinnedMesh — ECS component for entities that have a deformable mesh.
// Points to a shared Skeleton asset (bone hierarchy + inverse bind matrices)
// and holds per-entity bone palette (the final mat4[] sent to the GPU each
// frame). The Animator system writes to skinMatrices; the renderer reads it.
//
// Why separate from MeshRenderer?
//   Static meshes (the 90%+ common case) carry zero animation overhead.
//   Only entities with both MeshRenderer + SkinnedMesh enter the skinned path.
struct SkinnedMesh {
    SkeletonHandle skeleton;

    // Per-entity bone palette — kMaxBones * 16 floats (mat4[128]).
    // Populated each frame by the Animator system (Phase 3).
    // Until then the renderer can use identity matrices for a bind-pose preview.
    static constexpr int kMaxBones    = 128;
    static constexpr int kMatrixSize  = kMaxBones * 16;
    float skinMatrices[kMatrixSize]   = {};

    // Quick check — is the palette populated?
    bool hasSkinMatrices = false;
};
