#pragma once
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

#include <cassert>
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

    // gpu:: types since G1c. A third-party IRenderPipeline can now be declared
    // against this header without a graphics API in scope — which was the
    // point of the seam, and was not true while these were bgfx handles.
    gpu::TextureHandle whiteTex;
    gpu::TextureHandle flatNormalTex;

    // Engine owns the cursor; allocView() bumps it past the reserved range.
    //
    // gpu::ViewId is a plain uint16 alias, NOT an opaque handle: view ids are
    // compared, incremented and indexed all over the passes, and wrapping them
    // would be ceremony without a defect to point at. Aliasing it does not
    // prejudge what replaces the concept — under the RHI, passes declare their
    // own ordering through the render graph and view ids stop existing
    // (docs/rhi/design-axioms.md axiom 4).
    gpu::ViewId* viewCursor = nullptr;
    gpu::ViewId shadowViewId = 0; // reserved depth-from-light pass

    // ── The ceiling allocView must stay below ───────────────────────────────
    // bgfx runs views in id order. The renderer's output pass encodes each HDR
    // scene target for the display on views 190..192 — after every pipeline
    // view, before the editor's ImGui at 200. A pipeline view allocated at 190
    // or above would run AFTER the encode that should have consumed it, and its
    // draws would silently never reach the screen. This was stated in two
    // comments and enforced nowhere; the constant is shared with the renderer
    // (static_assert'd in renderer.h) and the limit is asserted below.
    static constexpr gpu::ViewId kFirstOutputView = 190;

    gpu::ViewId allocView() {
        if (!viewCursor) return 0;
        assert(*viewCursor < kFirstOutputView &&
               "allocView: view ids from 190 belong to the output pass");
        return (*viewCursor)++;
    }

    // ── Cooked shaders ──────────────────────────────────────────────────────
    // The library that turns a .cshader into a GPU program, and the resolved
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
