#pragma once

#include "core/handle.h"
#include <string>

// Animator — ECS component for entities that play skeletal animations.
// Phase 2 (GPU skinning) defines the data layout; Phase 3 adds the system
// that samples clips and writes to SkinnedMesh::skinMatrices each frame.
//
// Clip identity, in priority order:
//   clipPath  — a STANDALONE clip asset (e.g. a Mixamo clip FBX), bound to
//               this entity's skeleton by bone name at load (ClipLibrary).
//               Serialized as an AssetRef (uuid + project-relative path).
//   clipIndex — legacy: which clip inside the mesh's OWN source file.
// Either resolves to `clip`, the session-local handle sampling actually uses.
//
// Requires SkinnedMesh + MeshRenderer on the same entity.
struct Animator {
    AnimClipHandle clip;          // currently playing clip (session-local handle)
    std::string    clipPath;      // standalone clip source path ("" = use clipIndex)
    int            clipIndex = 0; // clip index within the mesh's source file
    float          time     = 0.0f;
    float          speed    = 1.0f;
    bool           playing  = false;
    bool           looping  = true;
};
