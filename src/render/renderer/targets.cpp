// ── Renderer: render targets and the view entry points ───────────────────────
//
// ONE concern: WHERE a frame lands. Three offscreen/backbuffer targets, their
// creation and destruction, and the three public calls that each pick a target,
// build a view over a world, and hand it to the pipeline.
//
// The entry points live with the targets rather than with the device because
// choosing a target IS all they do — the identical tail of every one of them is
// `buildView` (extract.cpp) then `m_pipeline->render`. Their differences are
// three lines of RenderTarget each, which is only visible when they sit together.
#include "render/renderer.h"


#include <bgfx/bgfx.h>
#include "render/gpu_bgfx.h"   // toBgfx / fromBgfx — renderer-internal

#include "core/logger.h"

void Renderer::destroyTargets() {
    gpu::destroy(m_sceneFB);
    gpu::destroy(m_sceneColorTex);
    gpu::destroy(m_sceneDepthTex);
    // The game FB must be DESTROYED, not just forgotten: forgetting the handles
    // leaked an FB + two textures per resize, and a continuous Scene View drag
    // exhausted the backend texture pool (handle 65535 / "Invalid texture
    // attachment" crash). bgfx::reset() never invalidates user-created handles,
    // so destroying here is safe.
    gpu::destroy(m_gameFB);
    gpu::destroy(m_gameColorTex);
    gpu::destroy(m_gameDepthTex);
    // No explicit invalidation any more: gpu::destroy nulls the handle it is
    // given, which is why a double destroy here is a no-op rather than a
    // use-after-free of a recycled slot.
}

void Renderer::createSceneFB(int w, int h) {
    // Drops the game FB too — it follows the scene FB size, so ensureGameFB
    // recreates it at the new one.
    destroyTargets();

    const uint16_t W = (uint16_t)w, H = (uint16_t)h;
    m_sceneColorTex = gpu::fromBgfx(bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT));
    m_sceneDepthTex = gpu::fromBgfx(bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT));
    bgfx::TextureHandle att[2] = { gpu::toBgfx(m_sceneColorTex),
                                   gpu::toBgfx(m_sceneDepthTex) };
    m_sceneFB = gpu::fromBgfx(bgfx::createFrameBuffer(2, att, false));

    m_sceneW = w; m_sceneH = h;
    bgfx::setViewFrameBuffer(kSceneView, gpu::toBgfx(m_sceneFB));
    bgfx::setViewClear(kSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x1a1a1aff, 1.0f, 0);
    bgfx::setViewRect(kSceneView, 0, 0, W, H);
    LOG_INFO("Renderer", "scene framebuffer %dx%d", w, h);
}

// Lazily created at the scene FB's size, because the editor only needs it once
// something asks for a game view. Returns false if it could not be made, and
// callers must then skip the view for this frame.
bool Renderer::ensureGameFB() {
    if (m_gameFB.valid()) return true;

    const uint16_t W = (uint16_t)m_sceneW, H = (uint16_t)m_sceneH;
    m_gameColorTex = gpu::fromBgfx(bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT));
    m_gameDepthTex = gpu::fromBgfx(bgfx::createTexture2D(W, H, false, 1,
        bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT));
    // Defensive: if texture allocation ever fails, skip the game view this frame
    // instead of asserting inside createFrameBuffer.
    if (!m_gameColorTex.valid() || !m_gameDepthTex.valid()) {
        gpu::destroy(m_gameColorTex);
        gpu::destroy(m_gameDepthTex);
        return false;
    }
    bgfx::TextureHandle gatt[2] = { gpu::toBgfx(m_gameColorTex),
                                    gpu::toBgfx(m_gameDepthTex) };
    m_gameFB = gpu::fromBgfx(bgfx::createFrameBuffer(2, gatt, false));
    return m_gameFB.valid();
}

void Renderer::renderScene(const float view[16], const float proj[16]) {
    RenderTarget target;
    target.fb         = m_sceneFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { 0.102f, 0.102f, 0.102f, 1.0f };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    RenderView    rv = buildView(*m_editorWorld, view, proj, target, kSceneView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void Renderer::renderGameView(const float view[16], const float proj[16],
                              const float clearColor[4], flecs::world* gameWorld) {
    if (!ensureGameFB()) return;

    RenderTarget target;
    target.fb         = m_gameFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    flecs::world& world = gameWorld ? *gameWorld : *m_editorWorld;
    RenderView    rv = buildView(world, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}

void Renderer::renderToBackbuffer(const float view[16], const float proj[16],
                                  const float clearColor[4], flecs::world* world) {
    RenderTarget target;
    target.fb         = BGFX_INVALID_HANDLE; // bgfx: invalid FB = backbuffer
    target.w          = (uint16_t)m_backW;
    target.h          = (uint16_t)m_backH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    flecs::world& w  = world ? *world : *m_editorWorld;
    RenderView    rv = buildView(w, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
}
