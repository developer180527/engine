#pragma once
#include <assetlib/mesh_asset.h>
#include <filesystem>

#include "io/asset_storage.h"
#include "render/asset_registry.h"

// MeshBinaryLoader — reads a .cooked mesh file and uploads it to the GPU.
// This is the runtime fast path: no Assimp, no parsing, just fread + bgfx::copy.
// Called instead of the Assimp AsyncLoader when a cooked version exists.
//
// Thread safety: safe to call from a worker thread — bgfx::copy() buffers the
// data, and bgfx handle creation happens on the main thread via drainOne().
namespace MeshBinaryLoader {
    // Load a .cooked file and add the result to AssetStorage.
    // Returns an invalid MeshHandle on failure.
    MeshHandle load(const std::filesystem::path& cookedPath,
                    const std::string&            sourcePath,
                    AssetStorage&                 storage);
}
