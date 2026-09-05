// ── null_renderer_test — the renderer that does nothing, doing it correctly ──
//
// NullRenderer replaced `if (!m_headless)` around every render call. That was a
// CORRECTNESS change, not a performance one — routing all 18 IRenderer methods
// through a virtual costs ~12 ns a tick, 0.000037% of a 30 Hz server tick
// (docs/rhi/headless.md §1). Nothing here measures anything.
//
// What it pins is the contract that made the change worth making. The scattered
// guards had already shipped a defect: `Renderer::frame()` was gated on having a
// window and `engineDrawSubmitBindRenderer()` was not, so a dedicated server's
// draw-submission list filled forever at ~480 KB/s and nothing ever drew it
// (src/runtime/docs/issues.md, 2026-08-10). One guard present, an adjacent one
// missing, silently.
//
// WHAT THE COMPILER ALREADY GUARANTEES, so this file does not: that NullRenderer
// implements every IRenderer method. They are pure virtual, so a new one is a
// build failure until it is implemented here. That is the whole reason axiom 5's
// "the second implementation rots" hazard does not apply — a do-nothing
// implementation has no answers to diverge, and a MISSING one will not link.
//
// WHAT THE COMPILER CANNOT SEE, and this file therefore does:
//   * the three queries whose null answer must still be MEANINGFUL, because a
//     caller uses the value rather than ignoring it;
//   * that a kit submitting draws on a server is accepted and discarded, not
//     dropped-with-an-error and not accumulated.
#include <cstdio>

#include <engine/engine_api.h>

#include "render/renderer_null.h"
#include "runtime/scripting/engine_api_binding.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("null_renderer_test: doing nothing, correctly\n");

    NullRenderer nr;
    IRenderer&   r = nr;      // exercised through the interface, as the runtime does

    // ── 1. Every method is callable and does nothing ───────────────────────
    // The runtime now calls these UNCONDITIONALLY. If any of them could fault
    // on a null renderer, the guards would have to come back — which is the
    // design this change exists to delete.
    {
        std::printf("\n-- 1. the do-nothing contract --\n");
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const float c[4]  = {0,0,0,1};

        r.setShadowResolution(2048);
        r.setShaderCacheRoot("/nonexistent");
        r.setDebugDraw(nullptr);
        r.setSimAlpha(0.5f);
        r.createSceneFB(1280, 720);
        r.resize(1920, 1080);
        r.renderScene(m, m);
        r.renderGameView(m, m, c, nullptr);
        r.renderToBackbuffer(m, m, c, nullptr);
        r.resetWorldCaches();
        r.frame();
        r.endFrame();
        r.shutdown();
        CHECK(true, "every render entry point ran without a device");

        // Order-independence matters: the runtime's teardown path calls
        // shutdown() and the frame path may still end a frame afterwards.
        r.frame();
        r.endFrame();
        CHECK(true, "and again after shutdown(), in any order");
    }

    // ── 2. frame() and endFrame() are BOTH real ────────────────────────────
    // The leak's shape: frame() presents AND resets the submission list, so
    // gating it on having a window also gated the reset. The fix is that the
    // runtime no longer chooses — it calls frame() always, and a null renderer
    // ends the frame just as the real one does.
    {
        std::printf("\n-- 2. no branch to get wrong --\n");
        r.frame();
        CHECK(r.submittedDrawCount() == 0,
              "after frame(), nothing is pending (%u)", r.submittedDrawCount());
        r.endFrame();
        CHECK(r.submittedDrawCount() == 0, "after endFrame(), still nothing");
    }

    // ── 3. The three queries whose value is READ ───────────────────────────
    // Everything else may do nothing. These cannot: a caller uses the answer,
    // so each is a decision recorded in renderer_null.h and asserted here.
    {
        std::printf("\n-- 3. the meaningful answers --\n");
        CHECK(r.homogeneousDepth() == false,
              "homogeneousDepth() is false — assume [0,1] clip depth "
              "(D3D/Metal/Vulkan), and nothing the SIMULATION reads depends on it");
        CHECK(r.sceneW() == 0 && r.sceneH() == 0,
              "scene size is 0x0 — 'there is no framebuffer', which is true; a "
              "plausible fake size would invite an aspect ratio off a surface "
              "that does not exist");
    }

    // ── 4. Diagnostics report "nothing", not garbage ───────────────────────
    {
        std::printf("\n-- 4. diagnostics --\n");
        CHECK(r.pipeline() == nullptr, "no pipeline attached");
        CHECK(r.lodCensus().empty(), "LOD census is empty");
        CHECK(!r.sceneColorTexture().valid() && !r.gameColorTex().valid(),
              "no scene or game colour texture");
        CHECK(r.droppedExternalDraws() == 0,
              "and ZERO dropped draws — non-zero means 'the frame is "
              "incomplete', which on a server would be a lie");
    }

    // ── 5. A kit drawing on a server ───────────────────────────────────────
    // The actual regression. Kit code is the same on a client and a server: a
    // particle system calls engineDrawSubmitMesh either way. On a server that
    // must be ACCEPTED AND DISCARDED — not an error, and above all not
    // accumulated, which is precisely what leaked.
    {
        std::printf("\n-- 5. a kit submitting on a server --\n");
        engineDrawSubmitBindRenderer(&nr);
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

        for (int i = 0; i < 10000; ++i)
            engineDrawSubmitMesh(1u, 1u, m);

        CHECK(engineDrawSubmittedCount() == 0,
              "10 000 submissions accumulate NOTHING (%u) — the leak's exact "
              "shape, now structurally impossible", engineDrawSubmittedCount());

        r.endFrame();
        CHECK(engineDrawSubmittedCount() == 0, "and ending the frame is still clean");

        // Unbinding must be safe: the runtime does it during shutdown, and a
        // kit callback can still be in flight.
        engineDrawSubmitBindRenderer(nullptr);
        engineDrawSubmitMesh(1u, 1u, m);
        CHECK(engineDrawSubmittedCount() == 0,
              "submitting with NO renderer bound is a no-op, not a crash");
    }

    if (g_failures) {
        std::printf("\nnull_renderer_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nnull_renderer_test: ALL PASS\n");
    return 0;
}
