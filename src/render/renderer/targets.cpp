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
#include "render/renderer/output_pass.h"

#include "core/logger.h"

// A colour attachment plus an optional depth attachment, as one framebuffer.
// Returns false — having destroyed whatever it made — if any piece failed, so a
// caller can skip a view for a frame instead of asserting in createFrameBuffer.
static bool makeTarget(uint16_t w, uint16_t h, bgfx::TextureFormat::Enum colourFmt,
                       bool withDepth, gpu::TextureHandle& colour,
                       gpu::TextureHandle& depth, gpu::FrameBufferHandle& fb) {
    colour = gpu::fromBgfx(bgfx::createTexture2D(w, h, false, 1, colourFmt,
                                                 BGFX_TEXTURE_RT));
    if (withDepth)
        depth = gpu::fromBgfx(bgfx::createTexture2D(w, h, false, 1,
            bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT));
    if (!colour.valid() || (withDepth && !depth.valid())) {
        gpu::destroy(colour);
        gpu::destroy(depth);
        return false;
    }
    bgfx::TextureHandle att[2] = { gpu::toBgfx(colour), gpu::toBgfx(depth) };
    fb = gpu::fromBgfx(bgfx::createFrameBuffer(withDepth ? 2 : 1, att, false));
    return fb.valid();
}

void Renderer::destroyTargets() {
    // HDR and display halves both, for every view. A target forgotten rather
    // than destroyed leaks one set per resize (see below), and colour stage A
    // doubled how many there are to forget.
    gpu::destroy(m_sceneHdrFB);
    gpu::destroy(m_sceneHdrTex);
    gpu::destroy(m_sceneFB);
    gpu::destroy(m_sceneColorTex);
    gpu::destroy(m_sceneDepthTex);
    // The game FB must be DESTROYED, not just forgotten: forgetting the handles
    // leaked an FB + two textures per resize, and a continuous Scene View drag
    // exhausted the backend texture pool (handle 65535 / "Invalid texture
    // attachment" crash). bgfx::reset() never invalidates user-created handles,
    // so destroying here is safe.
    gpu::destroy(m_gameHdrFB);
    gpu::destroy(m_gameHdrTex);
    gpu::destroy(m_gameFB);
    gpu::destroy(m_gameColorTex);
    gpu::destroy(m_gameDepthTex);
    gpu::destroy(m_backHdrFB);
    gpu::destroy(m_backHdrTex);
    gpu::destroy(m_backHdrDepthTex);
    m_backHdrW = m_backHdrH = 0;
    // No explicit invalidation any more: gpu::destroy nulls the handle it is
    // given, which is why a double destroy here is a no-op rather than a
    // use-after-free of a recycled slot.
}

void Renderer::createSceneFB(int w, int h) {
    // Drops the game FB too — it follows the scene FB size, so ensureGameFB
    // recreates it at the new one.
    destroyTargets();
    m_sceneW = w; m_sceneH = h;
    // Before init() there is no device and no output pass to say which format
    // the HDR half takes; init() calls this again once both exist.
    if (!m_output) return;

    const uint16_t W = (uint16_t)w, H = (uint16_t)h;
    // The pipeline's target: linear light, float where the backend allows, with
    // the depth buffer. Then the DISPLAY target the output pass encodes into —
    // colour only, RGBA8, and the texture sceneColorTexture() hands the editor.
    bool ok = makeTarget(W, H, m_output->hdrFormat, /*depth*/ true,
                         m_sceneHdrTex, m_sceneDepthTex, m_sceneHdrFB);
    gpu::TextureHandle noDepth;
    ok = ok && makeTarget(W, H, bgfx::TextureFormat::RGBA8, /*depth*/ false,
                          m_sceneColorTex, noDepth, m_sceneFB);
    if (!ok) {
        LOG_ERROR("Renderer", "scene framebuffer %dx%d could not be created", w, h);
        destroyTargets();
    }

    bgfx::setViewFrameBuffer(kSceneView, gpu::toBgfx(m_sceneHdrFB));
    bgfx::setViewRect(kSceneView, 0, 0, W, H);
    // Target size: HDR colour (RGBA16F = 8 B/px) + depth (D24S8, 4) + display
    // (RGBA8, 4). Colour stage A added the display half and doubled the colour
    // half, so this is +8 B/px per view over the old single RGBA8 target —
    // worth knowing on the Intel UHD 630's 128 MB budget.
    LOG_INFO("Renderer", "scene framebuffer %dx%d (%.1f MB incl. HDR + display)",
             w, h, (double)w * h *
                 ((m_output->hdrFormat == bgfx::TextureFormat::RGBA16F ? 8 : 4) + 4 + 4)
                 / (1024.0 * 1024.0));
}

// Lazily created at the scene FB's size, because the editor only needs it once
// something asks for a game view. Returns false if it could not be made, and
// callers must then skip the view for this frame.
bool Renderer::ensureGameFB() {
    if (!m_output) return false;
    if (m_gameFB.valid() && m_gameHdrFB.valid()) return true;

    const uint16_t W = (uint16_t)m_sceneW, H = (uint16_t)m_sceneH;
    gpu::TextureHandle noDepth;
    const bool ok =
        makeTarget(W, H, m_output->hdrFormat, /*depth*/ true,
                   m_gameHdrTex, m_gameDepthTex, m_gameHdrFB)
        && makeTarget(W, H, bgfx::TextureFormat::RGBA8, /*depth*/ false,
                      m_gameColorTex, noDepth, m_gameFB);
    if (!ok) {
        gpu::destroy(m_gameHdrFB); gpu::destroy(m_gameHdrTex);
        gpu::destroy(m_gameFB);    gpu::destroy(m_gameColorTex);
        gpu::destroy(m_gameDepthTex);
    }
    return ok;
}

bool Renderer::ensureBackHdrFB() {
    if (!m_output) return false;
    if (m_backHdrFB.valid() && m_backHdrW == m_backW && m_backHdrH == m_backH)
        return true;
    gpu::destroy(m_backHdrFB);
    gpu::destroy(m_backHdrTex);
    gpu::destroy(m_backHdrDepthTex);
    if (!makeTarget((uint16_t)m_backW, (uint16_t)m_backH, m_output->hdrFormat,
                    /*depth*/ true, m_backHdrTex, m_backHdrDepthTex, m_backHdrFB))
        return false;
    m_backHdrW = m_backW; m_backHdrH = m_backH;
    return true;
}

void Renderer::renderScene(const float view[16], const float proj[16]) {
    if (!m_sceneHdrFB.valid()) return;

    RenderTarget target;
    target.fb         = m_sceneHdrFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    // The editor's grey, as the sRGB colour it was always meant to be; the
    // pipeline decodes clear colours into the linear target (opaque_pass.cpp).
    target.clearColor = { 0.102f, 0.102f, 0.102f, 1.0f };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    RenderView    rv = buildView(*m_editorWorld, view, proj, target, kSceneView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
    m_output->submit(kSceneOutputView, gpu::toBgfx(m_sceneHdrTex),
                     gpu::toBgfx(m_sceneFB), target.w, target.h);
}

void Renderer::renderGameView(const float view[16], const float proj[16],
                              const float clearColor[4], flecs::world* gameWorld) {
    if (!ensureGameFB()) return;

    RenderTarget target;
    target.fb         = m_gameHdrFB;
    target.w          = (uint16_t)m_sceneW;
    target.h          = (uint16_t)m_sceneH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    flecs::world& world = gameWorld ? *gameWorld : *m_editorWorld;
    RenderView    rv = buildView(world, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
    m_output->submit(kGameOutputView, gpu::toBgfx(m_gameHdrTex),
                     gpu::toBgfx(m_gameFB), target.w, target.h);
}

void Renderer::renderToBackbuffer(const float view[16], const float proj[16],
                                  const float clearColor[4], flecs::world* world) {
    // No longer straight to the backbuffer: the pipeline renders linear light
    // into an HDR target at backbuffer size, and the output pass encodes it onto
    // the backbuffer — the same two steps the editor's views take, so a shipped
    // game and the editor's game view produce the same pixels.
    if (!ensureBackHdrFB()) return;

    RenderTarget target;
    target.fb         = m_backHdrFB;
    target.w          = (uint16_t)m_backW;
    target.h          = (uint16_t)m_backH;
    target.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
    target.clearFlags = gpu::kClearColor | gpu::kClearDepth;

    flecs::world& w  = world ? *world : *m_editorWorld;
    RenderView    rv = buildView(w, view, proj, target, kGameView);
    RenderContext rc = makeContext();
    m_pipeline->render(rv, rc);
    m_output->submit(kBackOutputView, gpu::toBgfx(m_backHdrTex),
                     BGFX_INVALID_HANDLE, target.w, target.h);
}
