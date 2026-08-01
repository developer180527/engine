#pragma once
#include <bgfx/bgfx.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

#include <filesystem>

namespace dbg { class DebugDraw; }   // core/debug_draw.h — line collector
class ShaderLibrary;                 // render/shader/shader_library.h

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

    // ── Cooked shaders ──────────────────────────────────────────────────────
    // The library that turns a .cshader into a bgfx program, and the resolved
    // cooked path of the engine's standard forward shader. Both may be
    // null/empty: a project with no cooked shaders (or a tool that never ran a
    // cook) still boots, on the compiled-in blobs. See
    // docs/plans/renderer-audit-and-plan.md Phase 5.
    ShaderLibrary*        shaders = nullptr;
    std::filesystem::path standardShader;

    // Immediate-mode debug lines (kits/plugins via engineDraw*). Drawn into each
    // world view after its meshes; null when nothing queued / no collector.
    const dbg::DebugDraw* debugDraw = nullptr;
};
