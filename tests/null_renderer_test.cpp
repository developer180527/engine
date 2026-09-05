// ── null_renderer_test — the renderer that does nothing, doing it correctly ──
//
// NullRenderer replaced `if (!m_headless)` around every render call. That was a
// CORRECTNESS change, not a performance one — routing all 26 IRenderer methods
// through a virtual costs ~18 ns a tick, 0.00005% of a 30 Hz server tick, and
// most of them are boot-time anyway (docs/rhi/headless.md §1). Nothing here
// measures anything.
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

#include <memory>

#include <engine/engine_api.h>

#include "render/renderer_null.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
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

    // ── 0. The public API is safe BEFORE init() ────────────────────────────
    // m_renderer is a unique_ptr now, and the old `Renderer m_renderer` by value
    // was callable at any point in the lifecycle. resize(), sceneW/H(),
    // createSceneFB() and renderer() all dereference with no init guard, so the
    // ctor installs a NullRenderer and initRenderer() replaces it — "never null"
    // as a property of the type rather than an argument about call ordering.
    //
    // Our own boot path never exposed this. An EMBEDDER does: the host owns the
    // window and event loop (platform-embedder.md), so a resize callback landing
    // around a failed or in-progress init is exactly this hazard.
    {
        std::printf("\n-- 0. before init() --\n");
        EngineRuntime fresh;                       // constructed, never inited
        CHECK(fresh.sceneW() == 0 && fresh.sceneH() == 0,
              "sceneW/H() answer on an uninitialised runtime");
        fresh.resize(1920, 1080);
        fresh.createSceneFB(800, 600);
        CHECK(fresh.renderer().pipeline() == nullptr,
              "renderer() returns a usable IRenderer, not a null deref");
        CHECK(true, "resize() and createSceneFB() are safe pre-init");
    }

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

    // ── 5. THE REGRESSION, through the runtime that actually had it ────────
    //
    // The first version of this section asserted engineDrawSubmittedCount() == 0
    // against a NullRenderer that returned a hardcoded 0 — a literal compared to
    // itself. It passed at 10 000 iterations, at 10, and at zero, and it would
    // have passed with the original defect fully restored, because it never went
    // near the code that had the bug. Review caught it.
    //
    // The bug was never in NullRenderer. It was in the RUNTIME's branch:
    //
    //     if (!m_headless) m_renderer.frame();   // presents AND clears
    //     else             m_renderer.endFrame();
    //
    // frame() both presented and reset the submission list, so gating it on
    // having a window also gated the reset — while engineDrawSubmitBindRenderer()
    // was ungated. A server accumulated forever at ~480 KB/s.
    //
    // So this boots a REAL headless EngineRuntime, submits through the REAL C
    // API a kit would use, and asserts the count goes up and then comes back to
    // zero across EngineRuntime::frameEnd(). Removing the unconditional call in
    // runtime_frame.cpp fails it.
    {
        std::printf("\n-- 5. a kit submitting on a headless runtime --\n");

        EngineConfig cfg;
        cfg.openAssetDatabase = false;
        cfg.autoDetectProject = false;

        EngineRuntime rt;
        if (!rt.init(cfg, std::make_unique<HeadlessPlatform>())) {
            std::printf("  FAIL  headless runtime init\n");
            return 1;
        }
        CHECK(rt.headless(), "the runtime is headless (no window handle)");

        // boot binds the runtime's OWN renderer, so the C API below lands on the
        // same object frameEnd() will clear. That coupling is the thing under
        // test; binding a local fake would prove nothing about the runtime.
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        for (int i = 0; i < 1000; ++i)
            engineDrawSubmitMesh(1u, 1u, m);

        const uint32_t pending = engineDrawSubmittedCount();
        CHECK(pending == 1000,
              "1000 kit submissions are COUNTED on the headless path (%u) — a "
              "count that is always zero cannot tell 'cleared' from 'never "
              "counted', which is how the first version of this test fooled "
              "itself", pending);

        rt.frameEnd();
        CHECK(engineDrawSubmittedCount() == 0,
              "and EngineRuntime::frameEnd() clears them (%u) — the leak was "
              "that nothing on this path ever did",
              engineDrawSubmittedCount());

        // Two more frames: the invariant is per-frame, not once.
        for (int f = 0; f < 2; ++f) {
            for (int i = 0; i < 500; ++i) engineDrawSubmitMesh(1u, 1u, m);
            CHECK(engineDrawSubmittedCount() == 500,
                  "frame %d accumulates again (%u)", f, engineDrawSubmittedCount());
            rt.frameEnd();
            CHECK(engineDrawSubmittedCount() == 0, "frame %d ends clean", f);
        }

        rt.shutdown();
    }

    // ── 6. Unbinding is safe ───────────────────────────────────────────────
    // The runtime unbinds during teardown and a kit callback can still be in
    // flight, so the C API must tolerate a null renderer.
    {
        std::printf("\n-- 6. after unbind --\n");
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
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
