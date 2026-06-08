#pragma once
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"

// Bundles all asset registries into one struct so importers don't need
// growing parameter lists when new asset types are added.
// Precursor to the full AssetManager (later milestone).
struct AssetStorage {
    AssetRegistry&    meshes;
    TextureRegistry&  textures;
    MaterialRegistry& materials;

    // Animation registries — optional (nullptr when not wired).
    // Importers that detect skeletal data check for non-null before storing.
    SkeletonRegistry* skeletons = nullptr;
    AnimClipRegistry* clips     = nullptr;
};
