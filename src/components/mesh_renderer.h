#pragma once
#include "core/handle.h"

// MeshRenderer component — tells the renderer which mesh to draw for this entity.
// materialOverride: when valid, overrides the mesh's shared material with a
// per-entity copy. Use the inspector's "Make Unique" to create an override.
// Invalid (default) = use mesh->material (shared across all instances).
struct MeshRenderer {
    MeshHandle     mesh;
    MaterialHandle materialOverride; // invalid = use mesh->material
};
