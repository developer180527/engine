#pragma once
#include <cstdint>
#include "core/handle.h"

// MeshRenderer component — tells the renderer which mesh to draw for this entity.
// materialOverride: when valid, overrides the mesh's shared material with a
// per-entity copy. Use the inspector's "Make Unique" to create an override.
// Invalid (default) = use mesh->material (shared across all instances).
struct MeshRenderer {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    MeshHandle     mesh;
    MaterialHandle materialOverride; // invalid = use mesh->material
};
