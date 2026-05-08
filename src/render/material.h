#pragma once
#include "core/handle.h"

// Material holds rendering parameters for a mesh.
// For now: base color texture + factor. More PBR params (roughness,
// metallic, normal map) added later when the shader supports them.
struct Material {
    TextureHandle baseColorTexture;            // invalid = no texture
    float         baseColorFactor[4] = {1,1,1,1};
    bool          doubleSided        = false;

    bool hasTexture() const { return baseColorTexture.valid(); }
};
