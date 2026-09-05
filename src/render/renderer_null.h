#pragma once
#include <atomic>

#include "render/renderer_interface.h"

// ── NullRenderer — does nothing, correctly ──────────────────────────────────
//
// The renderer for a process with no GPU: unit tests, the cook worker,
// engine_host in a CI container, a dedicated server. Unreal's equivalent is
// FNullDynamicRHI, selected by `-nullrhi` — "Use null rendering hardware
// interface to run UE headless" (docs/rhi/headless.md §3.1).
//
// It replaces `if (!m_headless)` scattered through the runtime. That mattered
// for correctness rather than speed: one guard present and an adjacent one
// missing leaked a dedicated server's draw-submission list at ~480 KB/s,
// forever, silently (src/runtime/docs/issues.md, 2026-08-10). There is no
// guard here to forget.
//
// HEADER-ONLY AND INLINE, deliberately. Every body is empty or a constant, so
// there is nothing to hide in a .cpp and nothing that could develop behaviour
// out of sight. If a method here ever needs a .cpp, that is the signal it has
// stopped being a null implementation.
//
// WHAT THIS DOES NOT DO: make a lean server binary. The real Renderer still
// links, because the runtime is compiled once. Excluding the render TUs is a
// build-target problem (docs/rhi/phases.md G1c step B), and it is that step —
// not this one — that satisfies "engine_runtime links without bgfx".
struct NullRenderer final : IRenderer {
    // ── Lifecycle ──────────────────────────────────────────────────────────
    // init() returns TRUE. A null renderer initialising successfully is the
    // honest answer: the runtime asked for a renderer and got a working one
    // that draws nothing. Returning false would read as "no renderer
    // available" and send callers down an error path that does not apply.
    bool init(void*, int, int, flecs::world&,
              AssetRegistry&, TextureRegistry&, MaterialRegistry&,
              SkeletonRegistry&) override { return true; }
    void shutdown() override {}

    void resize(int, int) override {}
    void createSceneFB(int, int) override {}

    void setShadowResolution(uint32_t) override {}
    void setShaderCacheRoot(const std::filesystem::path&) override {}
    void setDebugDraw(const dbg::DebugDraw*) override {}
    void setSimAlpha(float) override {}

    // frame() presents nothing and then ends the frame, exactly as the real
    // Renderer::frame() does. That equivalence is the point: the runtime calls
    // frame() unconditionally and must not have to know which renderer it holds.
    void frame() override { endFrame(); }
    void endFrame() override { m_submitted.store(0, std::memory_order_relaxed); }

    void renderScene(const float[16], const float[16]) override {}
    void renderGameView(const float[16], const float[16], const float[4],
                        flecs::world*) override {}
    void renderToBackbuffer(const float[16], const float[16], const float[4],
                            flecs::world*) override {}

    // COUNTED, then discarded at frame end — the same observable lifecycle the
    // real renderer has, minus the GPU and minus storing anything.
    //
    // Counting is not decoration. `submittedDrawCount()` is what makes the
    // 480 KB/s leak observable at all: the defect was that nothing on the
    // headless path ever cleared the pending list, and a count that is always
    // zero cannot distinguish "cleared" from "never counted". Returning a
    // constant here would make tests/null_renderer_test.cpp §5 compare a
    // literal against itself, which is exactly what review caught it doing.
    //
    // Atomic because kits submit from the job pool (engineJobsParallelFor), the
    // same reason the real Renderer takes a lock. Nothing is stored, so there is
    // no buffer to overflow and no cap is needed — which is why
    // droppedExternalDraws() below is honestly 0 rather than merely unimplemented.
    void submitDraw(MeshHandle, MaterialHandle, const float[16]) override {
        m_submitted.fetch_add(1, std::memory_order_relaxed);
    }

    // ── The three whose value is actually read ─────────────────────────────
    // Every other method above can do nothing. These cannot: a caller uses the
    // answer, so each is a decision.

    // FALSE = the OpenGL-style [-1,1] clip-space depth convention is NOT in
    // use, i.e. assume [0,1] (Direct3D/Metal/Vulkan). Only ever reaches
    // camera_util's projection math, whose output a null renderer discards —
    // verified: EngineRuntime::tick() reads it solely to build a matrix it
    // then hands to renderToBackbuffer(). Nothing the SIMULATION observes
    // depends on it, so this cannot desync a server from its clients.
    bool homogeneousDepth() const override { return false; }

    // ZERO, meaning "there is no scene framebuffer" — which is true, and is
    // what a caller sizing a viewport to it should see. A plausible-looking
    // fake size would be worse: it invites a caller to compute an aspect ratio
    // from a surface that does not exist, and to get a plausible-looking wrong
    // answer instead of an obvious zero.
    //
    // An earlier version of this comment also claimed "callers already handle 0
    // (the runtime falls back to 16:9 when height is 0)". That fallback is real
    // but it is in a DIFFERENT path — runtime_frame.cpp guards m_height, the
    // window size, not sceneH(). The runtime's only uses of sceneW/H are two
    // pass-through accessors. The real consumers are in editor_app.h and they
    // disagree with each other: the game-view path guards `sceneW() > 0`, the
    // scene-view path divides unguarded. Both are unreachable today (the editor
    // never runs headless) and the scene-view one is now guarded anyway, but the
    // citation was describing a protection that was not there.
    int sceneW() const override { return 0; }
    int sceneH() const override { return 0; }

    // ── Diagnostics: "nothing to report" is the correct answer ─────────────
    uint32_t submittedDrawCount() const override {
        return m_submitted.load(std::memory_order_relaxed);
    }
    uint32_t droppedExternalDraws() const override { return 0; }
    const IRenderPipeline* pipeline() const override { return nullptr; }
    LodCensus lodCensus() const override { return {}; }
    gpu::TextureHandle sceneColorTexture() const override { return {}; }
    gpu::TextureHandle gameColorTex() const override { return {}; }

    void resetWorldCaches() override {}

private:
    // Pending submissions THIS frame. Never grows across frames — that is
    // the invariant the leak violated, and the only state this class has.
    std::atomic<uint32_t> m_submitted{0};
};
