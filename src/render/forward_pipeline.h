#pragma once
// Default render pipeline: single-pass forward PBR. Owns its shader program +
// uniforms — and NOTHING ELSE. Culling, sort keys, visibility and light packing
// live in render/world (rworld::), so a project that swaps this pipeline to
// change how surfaces LOOK inherits the machinery instead of reimplementing it.
// That was the whole reason IRenderPipeline was a customization point nobody
// could use; see docs/architecture/renderer-architecture.md §3 and §5.
#include "render/render_pipeline.h"
#include "render/world/light_packing.h"
#include "render/world/visibility.h"
#include "render/world/draw_sort.h"   // the expanded draw list is re-sorted here
#include "render/shader/shader_library.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "core/logger.h"           // renderer diagnostics reach the EDITOR console
#include "core/debug_draw.h"          // dbg::DebugDraw / DebugVertex (line pass)
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>


class ForwardPipeline final : public IRenderPipeline {
public:
    const char* name() const override { return "Forward PBR"; }

    // Must be called BEFORE onAttach() — the shadow map is created there and
    // is not resized afterwards. Values are validated at the project layer
    // (clamped, rounded to a power of two); this only guards against a caller
    // that bypasses it.
    void setShadowResolution(uint32_t px) {
        if (px < 256)  px = 256;
        if (px > 8192) px = 8192;
        SHADOW_SIZE = (uint16_t)px;
    }

    void onAttach(RenderContext& attachCtx) override;

    void onDetach() override;

    // Drain the debug-line collector into view `id` (its view-transform is
    // already set). Depth-tested against the scene so lines occlude naturally;
    // no writes to depth. One transient buffer, one submit.
    void submitDebugLines(bgfx::ViewId id, RenderContext& ctx);

    void render(const RenderView& v, RenderContext& ctx) override;

    // PUBLIC deliberately, matching IRenderPipeline. It was private here, which
    // compiles (an override may narrow access) but means the counters are
    // reachable only through a base pointer — including for tests, whose whole
    // reason to exist is reading them. See tests/render_pipeline_test.cpp.
    const rdiag::SubmitStats& submitStats() const override { return m_submitStats; }

private:
    // The program a data-driven material wants: its own shader, its own
    // feature mask. Cached per (path, mask, profile) by ShaderLibrary, so this
    // is a hash lookup after the first draw and not a per-draw load.
    bgfx::ProgramHandle programFor(const Material& mat, RenderContext& ctx) {
        if (!ctx.shaders || mat.shaderName.empty()) return BGFX_INVALID_HANDLE;
        const auto path = ctx.shaders->resolveByName(mat.shaderName);
        if (path.empty()) {
            if (m_missingShaders.insert(mat.shaderName).second)
                LOG_WARN("Renderer",
                         "material wants shader \"%s\", which is not in the cooked "
                         "cache — falling back to the fixed path",
                         mat.shaderName.c_str());
            return BGFX_INVALID_HANDLE;
        }
        return ctx.shaders->program(path, mat.featureMask);
    }

    void renderShadow(const RenderView& v, RenderContext& ctx,
                      const rworld::PackedLights& lights);

    // Per-draw encoder state (textures, state, transform, vertex buffer). The
    // material's uniforms are NOT here — see BoundMaterial.
    void bindDrawState(bgfx::TextureHandle base, bgfx::TextureHandle norm,
                       uint64_t state, const RenderItem& it);

    // ── R7: the last FIXED-PATH material bound this pass ────────────────────
    // Draws arrive sorted with material above mesh in the key, so consecutive
    // draws usually share one material and re-uploading its uniforms per draw is
    // pure waste. Only the UNIFORMS are skippable: in bgfx a uniform VALUE
    // persists until something overwrites it, whereas setTexture/setState/
    // setTransform/setVertexBuffer are per-draw encoder state that submit()
    // discards — so those are re-issued every draw regardless, and skipping them
    // would render with whatever the previous draw left bound.
    //
    // Reset at the top of every render() and renderShadow(): a stale entry across
    // views would skip an upload the new view never made.
    struct BoundMaterial {
        uint32_t id = UINT32_MAX;        // material handle id; UINT32_MAX = none
        bgfx::TextureHandle base = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle norm = BGFX_INVALID_HANDLE;
        void reset() { id = UINT32_MAX; }
        bool holds(uint32_t mid) const { return id == mid && mid != UINT32_MAX; }
    };
    BoundMaterial m_boundMat;

    // True only while submitting an instanced run — see bind() and the run loop.
    bool m_instancing = false;

    // NOTE: culling moved to render/world/frustum.{h,cpp} and lighting's fixed
    // cap to rworld::kMaxLights. Replacing the fixed-cap forward path with
    // clustered forward is docs/architecture/renderer-architecture.md §2 — lights beyond the
    // cap are still dropped today.
    bgfx::ProgramHandle m_program              = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_instancedProgram     = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_instancedShadowProgram = BGFX_INVALID_HANDLE;
    // True when m_program came from a .cshader — i.e. ShaderLibrary owns it.
    bool                m_programFromAsset     = false;
    bgfx::ProgramHandle m_skinnedProgram       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_lineProgram          = BGFX_INVALID_HANDLE;   // debug lines
    bgfx::VertexLayout  m_lineLayout;
    bgfx::UniformHandle m_uBoneMatrices        = BGFX_INVALID_HANDLE;
    rworld::VisibleSet  m_shadowVisible;   // light-space, culled + sorted
    rdiag::SubmitStats  m_submitStats{};

    // ── Hard ceiling on submits per frame ───────────────────────────────────
    // bgfx's Metal backend commits per-draw uniform data into a FIXED 8 MB
    // scratch buffer (renderer_mtl.cpp:23, UNIFORM_BUFFER_SIZE) at an advancing
    // offset, with NO bounds check. This pipeline writes ~1 KB of uniform traffic
    // per draw (vertex + fragment blocks, each alignment-padded), so the buffer
    // is exhausted at ~8192 draws and bgfx then writes past the end of the Metal
    // allocation: SIGSEGV inside _platform_memmove, stack in
    // RendererContextMtl::commit. Found with ASan against a generated stress
    // scene (scripts/gen_stress_scene.py); measured 8 000 objects renders 320
    // frames clean and 8 100 crashes.
    //
    // Handing bgfx a frame it cannot survive is not an option, so stop and SAY
    // SO. Dropping draws makes a frame visibly incomplete, which is bad — but a
    // loud incomplete frame beats a segfault, and `drawsDropped` makes it
    // impossible to miss. The real fix is fewer uniform bytes per draw
    // (instancing R5, material-bind dedup R7), not a bigger buffer.
    // 4096, not "just under 8192". Two reasons. The per-draw uniform cost is
    // MEASURED at ~1035 B (8 MB / the ~8100-draw empirical threshold), not the
    // round 1 KB it looks like, so a cap near the limit still overflows — 7800
    // draws was tried and still crashed. And this engine's own budget target is
    // 500 draw calls (docs/plans/renderer-audit-and-plan.md), so 4096 is already
    // 8x the envelope: any frame reaching it is far outside what the renderer is
    // designed for, and clamping there costs nothing real while leaving ~4 MB of
    // headroom against a limit we do not control.
    static constexpr uint32_t kMaxDrawsPerFrame = 4096;

    // ── Once-per-run latches for silent degradations ────────────────────────
    // Every renderer log is latched or already deduped. A diagnostic inside the
    // submit loop that fired per draw would cost more than the thing it reports
    // and bury the log it belongs in — which is why these are bools and not
    // if-statements on a counter.
    bool m_warnedNoInstancing  = false;   // backend reports no instancing support
    bool m_warnedInstanceBuf    = false;  // per-frame instance buffer ran out

    // Loud ONCE per run: a per-frame message would bury the log it belongs in.
    bool m_warnedDrawCeiling = false;
    bool drawBudgetExhausted() {
        const uint32_t total = m_submitStats.draws + m_submitStats.shadowDraws;
        if (total < kMaxDrawsPerFrame) return false;
        ++m_submitStats.drawsDropped;
        if (!m_warnedDrawCeiling) {
            m_warnedDrawCeiling = true;
            // ERROR, not warning: geometry is missing from what the player sees.
            // Through the logger so it reaches the editor console — on stderr it
            // was invisible to anyone not watching a terminal.
            LOG_ERROR("Renderer",
                "DRAW CEILING HIT: %u draws this frame (limit %u). The Metal "
                "backend's uniform scratch is a fixed 8 MB and this pipeline uses "
                "~1 KB per draw, so bgfx writes past the end at ~8192 draws "
                "(SIGSEGV in RendererContextMtl::commit). Refusing to submit more "
                "— THIS FRAME IS INCOMPLETE. Reduce draws or per-draw uniforms.",
                total, kMaxDrawsPerFrame);
        }
        return true;
    }
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
    // Engine-driven texture presence, kept OUT of u_params so a material's
    // uniform block (which owns its whole vec4) cannot zero it.
    bgfx::UniformHandle m_uTexFlags    = BGFX_INVALID_HANDLE;
    // Shader names already reported missing — otherwise one bad material logs
    // per draw, per frame.
    std::set<std::string> m_missingShaders;
    // Materials already reported as bound through the data-driven path.
    std::set<std::string> m_boundDataDriven;
    bgfx::UniformHandle m_uColorFactor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uCamPos      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sNormalMap   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLights      = BGFX_INVALID_HANDLE;
    // How the cull gets threads. rworld is deliberately runtime-free, so it takes
    // a dispatcher rather than including the job facade — see visibility.h. Built
    // once, in onAttach, because a std::function constructed per view per frame
    // would allocate in the hot path.
    rworld::ParallelForFn m_cullParallel;

    // Submesh-expanded draw list: one entry per (item, range), each carrying that
    // range's OWN material in its key, then re-sorted so ranges group across items.
    // Only built when some visible item actually has submeshes — a scene of
    // single-material meshes uses m_visible.draws directly and pays nothing.
    std::vector<rworld::VisibleDraw> m_drawList;
    std::vector<rworld::VisibleDraw> m_drawScratch;   // radix ping-pong

    rworld::VisibleSet m_visible;   // reused across frames for its capacity
    // Shadow-map edge length. NOT a constant any more: it is the single
    // biggest VRAM lever in the engine (cost is size² × 4 bytes for D32F), and
    // the right value depends on the machine the GAME ships to. Set from
    // project.json's "graphics.shadowResolution" via setShadowResolution()
    // BEFORE init(). The old hardcoded 4096 cost 64 MB — by itself 90% of a
    // shipped game's GPU footprint, more than every mesh and texture combined.
    uint16_t SHADOW_SIZE = 2048;   // 16 MB; see ProjectContext::Graphics
    static constexpr float    SHADOW_ORTHO_RADIUS = 22.0f;  // half-width of the light box (world units) — tighten to fit the scene
    static constexpr float    SHADOW_EYE_DIST     = 60.0f;  // light-camera distance from the box center
    static constexpr float    SHADOW_NEAR         = 20.0f;  // tightened depth range -> better precision -> less acne
    static constexpr float    SHADOW_FAR          = 120.0f;
    static constexpr float    SHADOW_BIAS         = 0.0025f; // normalized-depth slope bias base
    bgfx::ProgramHandle     m_shadowProgram = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_shadowFB      = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_shadowMap     = BGFX_INVALID_HANDLE;
    float m_lightView[16] = {0};
    float m_lightProj[16] = {0};
    bool  m_hasShadowCaster = false;
    bgfx::UniformHandle m_sShadowMap    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uShadowMtx    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uShadowParams = BGFX_INVALID_HANDLE;
    float m_shadowMtx[16] = {0};
    int   m_shadowLightIndex = 0;
};
