#pragma once
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

// Bundles all asset registries into one struct so importers don't need
// growing parameter lists when new asset types are added.
// Precursor to the full AssetManager (later milestone).
struct AssetStorage {
    AssetRegistry&    meshes;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
};
