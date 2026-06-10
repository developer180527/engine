#pragma once

#include "core/handle.h"

// Animator — ECS component for entities that play skeletal animations.
// Phase 2 (GPU skinning) defines the data layout; Phase 3 adds the system
// that samples clips and writes to SkinnedMesh::skinMatrices each frame.
//
// Requires SkinnedMesh + MeshRenderer on the same entity.
struct Animator {
    AnimClipHandle clip;          // currently playing clip (session-local handle)
    int            clipIndex = 0; // which clip in the mesh's source file —
                                  // the cross-session identity; the loader
                                  // resolves it to `clip` after import
    float          time     = 0.0f;
    float          speed    = 1.0f;
    bool           playing  = false;
    bool           looping  = true;
};
