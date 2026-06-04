#pragma once
#include <bgfx/bgfx.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

// Shared services handed to a pipeline: resource registries (so it MAY resolve
// handles itself), fallback textures, and a collision-free view-id allocator so
// pass view-ids never clash with the editor's ImGui/blit views.
struct RenderContext {
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;

    bgfx::TextureHandle whiteTex      = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle flatNormalTex = BGFX_INVALID_HANDLE;

    // Engine owns the cursor; allocView() bumps it past the reserved range.
    bgfx::ViewId* viewCursor = nullptr;
    bgfx::ViewId shadowViewId = 0; // reserved depth-from-light pass
    bgfx::ViewId allocView() { return viewCursor ? (*viewCursor)++ : 0; }
};
