#pragma once
#include "core/handle.h"

// Material holds rendering parameters for a mesh.
// For now: base color texture + factor. More PBR params (roughness,
// metallic, normal map) added later when the shader supports them.
struct Material {
    TextureHandle baseColorTexture;            // invalid = no texture
    TextureHandle normalMapTexture;            // invalid = no normal map
    float         baseColorFactor[4] = {1,1,1,1};
    float         roughness          = 0.7f; // 0=mirror 1=fully rough
    float         metallic           = 0.0f; // 0=dielectric 1=metal
    bool          doubleSided        = false;

    bool hasTexture() const { return baseColorTexture.valid(); }
};
