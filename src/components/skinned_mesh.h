#pragma once

#include "core/handle.h"
#include <cstdint>

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

    // The bone palette lives in anim::skinPalettes(), NOT here. This used to be
    // `float skinMatrices[128*16]` inline, which made the component 8 200 bytes
    // — 64 cache lines, larger than a page.
    //
    // The cost was iteration, not animation. The renderer's extraction query
    // takes SkinnedMesh as a term and reads five bytes per entity (the handle
    // and the flag); every byte the component carried was stride it paid for
    // and never looked at. Measured on the real component, reading the handle
    // cost 15.8x more at 20 000 entities with the palette inline, and the ratio
    // GREW with entity count — a cache cliff, not extra work.
    //
    // kNoSlot until the animator first writes this entity; resolve with
    // anim::skinPalettes().at(paletteSlot), which returns null for kNoSlot so
    // an unanimated entity needs no special case.
    static constexpr int      kMaxBones   = 128;
    static constexpr int      kMatrixSize = kMaxBones * 16;
    static constexpr uint32_t kNoSlot     = 0xFFFFFFFFu;
    uint32_t paletteSlot = kNoSlot;

    // Quick check — is the palette populated?
    bool hasSkinMatrices = false;
};
