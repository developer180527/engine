#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>

#include <flecs.h>

#include "core/handle.h"
#include "render/gpu.h"
#include "render/world/lod.h"   // rworld::kMaxLodLevels

class AssetRegistry;
class TextureRegistry;
class MaterialRegistry;
class SkeletonRegistry;
struct IRenderPipeline;
namespace dbg { class DebugDraw; }

// How many items last frame's extraction placed at each LOD level, plus how
// many hit a broken chain (a level whose mesh handle does not resolve, which
// falls back to a finer level). `level[0]` counts only entities that HAVE an
// LOD chain and stayed at full detail — items with no chain are not LOD
// decisions and are not counted, so all-zero means "nothing in this scene is
// authored with LODs", not "LOD is broken".
//
// Moved out of Renderer when IRenderer was introduced: a diagnostic the
// interface reports cannot live inside one implementation of it.
struct LodCensus {
    uint32_t level[rworld::kMaxLodLevels] = {};
    uint32_t broken = 0;
    uint64_t trisDrawn = 0;    // triangles the chosen levels submitted
    uint64_t trisFull  = 0;    // triangles level 0 everywhere would submit
    bool empty() const {
        for (uint32_t n : level) if (n) return false;
        return broken == 0;
    }
};

// ── IRenderer — what the runtime is allowed to ask a renderer to do ─────────
//
// EngineRuntime drives rendering through this and never names a concrete
// renderer. Two implementations:
//
//   Renderer      (render/renderer.h)      the real one, owns a GPU device
//   NullRenderer  (render/renderer_null.h) does nothing, correctly
//
// ── Why this exists, since it is NOT for performance ────────────────────────
// This interface has 26 methods. Routing EVERY one of them through a virtual
// once per tick costs ~18 ns — 0.00005% of a 30 Hz server tick
// (docs/rhi/headless.md §1). The real per-tick number is far smaller, since
// most of these are lifecycle and configuration that run at boot or never.
// Nobody should defend or attack this design on speed; the number is noise
// either way, which is the only reason the loose version of it survived review.
//
// It exists because the alternative was `if (!m_headless)` scattered across
// the runtime, and that design has already shipped a defect: `frame()` was
// guarded and `engineDrawSubmitBindRenderer()` was not, so a dedicated server
// filled its draw-submission list forever at ~480 KB/s and nothing ever drew
// it (src/runtime/docs/issues.md, 2026-08-10). One guard present, an adjacent
// one missing, silently. A null object cannot have that bug, because there is
// no guard to forget — the call always happens and always lands somewhere.
//
// ── PURE VIRTUAL ON PURPOSE ─────────────────────────────────────────────────
// Every method here is pure. Adding one is then a COMPILE ERROR until
// NullRenderer implements it, which is what stops the second implementation
// rotting — the hazard design-axioms.md axiom 5 names for the CPU-driven cull
// path. That hazard is about an implementation producing different ANSWERS; a
// do-nothing implementation has no answers to diverge. The exception is the
// three queries whose null value must still be meaningful, marked below.
//
// ── WHAT THIS IS NOT ────────────────────────────────────────────────────────
// It is not a swap point for third-party renderers, and not an ABI. It is a
// null-object seam so one binary can run without a GPU, which is exactly what
// Unreal's `-nullrhi` is for. A LEAN SERVER is a different problem and this
// does not solve it: under a null renderer the real renderer still links and
// still builds draw lists into nothing. That needs a build target that
// excludes the render TUs (docs/rhi/phases.md G1c step B).
struct IRenderer {
    virtual ~IRenderer() = default;

    // ── Lifecycle ──────────────────────────────────────────────────────────
    virtual bool init(void* nwh, int width, int height,
                      flecs::world& editorWorld,
                      AssetRegistry& assets, TextureRegistry& textures,
                      MaterialRegistry& materials,
                      SkeletonRegistry& skeletons) = 0;
    virtual void shutdown() = 0;

    // ── Targets ────────────────────────────────────────────────────────────
    virtual void resize(int w, int h) = 0;
    virtual void createSceneFB(int w, int h) = 0;

    // ── Configuration ──────────────────────────────────────────────────────
    virtual void setShadowResolution(uint32_t px) = 0;
    virtual void setShaderCacheRoot(const std::filesystem::path& cacheRoot) = 0;
    virtual void setDebugDraw(const dbg::DebugDraw* dd) = 0;
    virtual void setSimAlpha(float a) = 0;

    // ── The frame ──────────────────────────────────────────────────────────
    // frame() presents; endFrame() is the DEVICE-FREE half that resets the
    // external submission list. Both are on the interface and both are called
    // unconditionally: the runtime no longer chooses between them, which is
    // the specific mistake that leaked 480 KB/s.
    virtual void frame() = 0;
    virtual void endFrame() = 0;

    virtual void renderScene(const float view[16], const float proj[16]) = 0;
    virtual void renderGameView(const float view[16], const float proj[16],
                                const float clearColor[4],
                                flecs::world* gameWorld) = 0;
    virtual void renderToBackbuffer(const float view[16], const float proj[16],
                                    const float clearColor[4],
                                    flecs::world* world) = 0;

    // ── Bring-your-own-system draw submission ──────────────────────────────
    // The ONE method here that is per-DRAW rather than per-frame: kit code
    // calls it from the job pool. Capped at 1 024 a frame, so a virtual costs
    // ~0.7 µs/frame worst case — still noise, but it is the only entry on this
    // interface where swappability.md §1's granularity rule applies at all.
    virtual void submitDraw(MeshHandle mesh, MaterialHandle material,
                            const float model[16]) = 0;

    // ── Queries whose NULL ANSWER MUST STILL BE MEANINGFUL ─────────────────
    // Unlike everything above, "do nothing" is not a valid implementation of
    // these three — a caller uses the value. NullRenderer's answers are chosen
    // and commented there rather than defaulted.
    virtual bool homogeneousDepth() const = 0;
    virtual int  sceneW() const = 0;
    virtual int  sceneH() const = 0;

    // ── Diagnostics ────────────────────────────────────────────────────────
    // Reported through the interface because the editor and engine_host read
    // them off whatever renderer the runtime holds. A null implementation
    // returning "nothing to report" is a correct answer to all of these.
    virtual uint32_t submittedDrawCount() const = 0;
    virtual uint32_t droppedExternalDraws() const = 0;
    virtual const IRenderPipeline* pipeline() const = 0;
    virtual LodCensus lodCensus() const = 0;
    virtual gpu::TextureHandle sceneColorTexture() const = 0;
    virtual gpu::TextureHandle gameColorTex() const = 0;

    // ── World lifetime ─────────────────────────────────────────────────────
    // Drop cached queries against the play-mode world — the runtime calls this
    // when that world is destroyed (sim stop).
    virtual void resetWorldCaches() = 0;
};
