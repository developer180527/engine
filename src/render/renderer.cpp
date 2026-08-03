// ── Renderer: pipeline ownership ─────────────────────────────────────────────
//
// ONE concern: which pipeline is attached, and the RenderContext handed to it.
// The Renderer's other three jobs live next door, one file each:
//
//   renderer/device.cpp   bgfx up and down (+ the Rendering-heap allocator)
//   renderer/targets.cpp  framebuffers and the three render* entry points
//   renderer/extract.cpp  ECS world → RenderView — the hot path
//
// The recurring hazard is all in this file: THE PIPELINE BUILDS ITS PROGRAMS AND
// TARGETS IN onAttach, ONCE. So every setting that those depend on has to
// re-attach when it changes after init, and both setters below exist only for
// that. Forgetting it does not fail loudly — it silently keeps the old programs,
// which is how the cooked-shader path once no-oped entirely.
#include "render/renderer.h"

#include "render/forward_pipeline.h"
#include "render/shader/shader_library.h"

Renderer::Renderer()  = default;
Renderer::~Renderer() = default;

void Renderer::setShadowResolution(uint32_t px) {
    if (px == m_shadowResolution) return;
    m_shadowResolution = px;
    if (!m_initialized) return;          // picked up by init()

    // The shadow map is created at attach time and never resized, so applying
    // a new size means re-attaching. Only a custom pipeline that is not the
    // default forward one is left alone — it owns its own targets and this
    // setting says nothing about them.
    if (auto* fp = dynamic_cast<ForwardPipeline*>(m_pipeline.get())) {
        fp->onDetach();
        fp->setShadowResolution(px);
        RenderContext rc = makeContext();
        fp->onAttach(rc);
    }
}

void Renderer::setShaderCacheRoot(const std::filesystem::path& cacheRoot) {
    if (!m_shaderLib) {                      // headless / pre-init
        m_shaderCacheRoot = cacheRoot;
        return;
    }
    m_shaderCacheRoot = cacheRoot;
    m_shaderLib->setSearchRoot(cacheRoot);
    if (!m_initialized || !m_pipeline) return;

    // openProject() runs after init(), so the pipeline already attached against
    // an empty cache and built the compiled-in fallback. Re-attach so the cooked
    // program actually gets used — without this the whole path silently no-ops
    // and every shader edit appears to do nothing.
    m_pipeline->onDetach();
    RenderContext rc = makeContext();
    m_pipeline->onAttach(rc);
}

void Renderer::setPipeline(std::unique_ptr<IRenderPipeline> pipeline) {
    if (m_initialized && m_pipeline) m_pipeline->onDetach();
    m_pipeline = std::move(pipeline);
    if (m_initialized && m_pipeline) {
        RenderContext rc = makeContext();
        m_pipeline->onAttach(rc);
    }
}

RenderContext Renderer::makeContext() {
    RenderContext rc{ *m_assets, *m_textures, *m_materials };
    rc.shaders = m_shaderLib.get();
    // Resolved by NAME from the cooked cache — a dist has no registry.
    if (m_shaderLib) rc.standardShader = m_shaderLib->resolveByName("standard");
    rc.whiteTex      = m_whiteTex;
    rc.flatNormalTex = m_flatNormalTex;
    rc.viewCursor    = &m_viewCursor;
    rc.shadowViewId  = kShadowView;
    rc.debugDraw     = m_debugDraw;
    return rc;
}

void Renderer::resetWorldCaches() {
    m_gameItemQuery.reset();
    m_gameLightQuery.reset();
}
