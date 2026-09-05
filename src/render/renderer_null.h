#pragma once
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

    void frame() override {}
    void endFrame() override {}

    void renderScene(const float[16], const float[16]) override {}
    void renderGameView(const float[16], const float[16], const float[4],
                        flecs::world*) override {}
    void renderToBackbuffer(const float[16], const float[16], const float[4],
                            flecs::world*) override {}

    // Accepted and discarded. NOT counted: submittedDrawCount() reports what is
    // pending presentation, and nothing here is ever pending. A kit that
    // submits on a server is behaving correctly and must not be told its work
    // was dropped — droppedExternalDraws() stays 0 for the same reason, since
    // non-zero there means "the frame is incomplete", which would be a lie.
    void submitDraw(MeshHandle, MaterialHandle, const float[16]) override {}

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
    // from a surface that does not exist. Callers already handle 0 (the
    // runtime falls back to 16:9 when height is 0).
    int sceneW() const override { return 0; }
    int sceneH() const override { return 0; }

    // ── Diagnostics: "nothing to report" is the correct answer ─────────────
    uint32_t submittedDrawCount() const override { return 0; }
    uint32_t droppedExternalDraws() const override { return 0; }
    const IRenderPipeline* pipeline() const override { return nullptr; }
    LodCensus lodCensus() const override { return {}; }
    gpu::TextureHandle sceneColorTexture() const override { return {}; }
    gpu::TextureHandle gameColorTex() const override { return {}; }

    void resetWorldCaches() override {}
};
