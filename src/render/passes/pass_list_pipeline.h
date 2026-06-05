#pragma once
#include "render/render_pipeline.h"        // IRenderPipeline, RenderView
#include "render/passes/i_render_pass.h"
#include <memory>
#include <vector>
// ── PassListPipeline ─────────────────────────────────────────────────────────
// SCAFFOLD — see docs/renderer-architecture.md.
//
// The migration target that replaces the monolithic ForwardPipeline. It IS an
// IRenderPipeline (so Renderer::setPipeline keeps working unchanged), but its
// body is just an ordered list of passes:
//
//   ShadowPass -> SkyPass -> OpaquePass -> TransparencyPass -> PostPass -> ResolvePass
//
// render():
//   1. build a PassContext from the RenderView + RenderContext
//   2. setup(fc)   for every enabled pass   (producers publish resources)
//   3. execute(fc) for every enabled pass   (consumers read them, submit draws)
//
// Deliberately a FLAT ordered list, NOT a frame graph. We add a graph (auto
// ordering from declared resource reads/writes) only once the PassContext hand-
// offs grow dependencies we can't keep correct by hand.
//
// BUILD ORDER (each step keeps the engine shippable + pixel-identical):
//   1. IRenderPass + PassContext (this scaffold).            [done]
//   2. RenderWorld (named extraction output).                [trigger: 2nd consumer]
//   3. ShadowPass  (move renderShadow here).                 [trigger: CSM]
//   4. OpaquePass + SkyPass (move the main submit + clear).
//   5. Swap Renderer's default pipeline to this.
//   6. Delete ForwardPipeline once parity is verified.
class PassListPipeline final : public IRenderPipeline {
public:
    void onAttach(RenderContext& ctx) override;                       // TODO
    void onDetach() override;                                         // TODO
    void render(const RenderView& view, RenderContext& ctx) override; // TODO
private:
    std::vector<std::unique_ptr<IRenderPass>> m_passes;
};
