#pragma once
#include "render/passes/i_render_pass.h"
// ── ShadowPass ───────────────────────────────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT DOES (today inside ForwardPipeline::renderShadow — to be MOVED here):
//   Renders scene depth from the first directional light with castShadows=true
//   into a depth map, builds the world->light matrix (+ crop), and publishes
//   shadowMap/shadowMatrix/shadowParams on PassContext for OpaquePass to sample.
//   Runs into the reserved shadow view (id 0) so it completes before the scene.
//
// EXTRACTION CHECKLIST (trigger to do this for real: CSM):
//   [ ] move renderShadow() body + shadow program/FB/uniforms here
//   [ ] onAttach(): create m_shadowMap / m_shadowFB / m_program
//   [ ] setup(): pick caster, build the ortho light matrix, set m_hasCaster
//   [ ] execute(): depth-only submit of all casters
//   [ ] publish shadowMap/shadowMatrix/shadowParams to PassContext
//
// FUTURE:
//   - Cascaded Shadow Maps: N cascades fitted to camera frustum slices, texel-
//     snapped, picked per-fragment by view depth. THIS is why shadows are their
//     own pass — cascades are N depth sub-passes.
//   - Spot/point shadows (perspective / cube maps); explicit "primary sun" when
//     several directional casters exist.
class ShadowPass final : public IRenderPass {
public:
    const char* name() const override { return "ShadowPass"; }
    void onAttach(RenderContext& ctx) override;   // TODO
    void onDetach() override;                      // TODO
    void setup(PassContext& fc) override;          // TODO: caster + light matrix
    void execute(PassContext& fc) override;        // TODO: depth-only submit
    bool enabled() const override { return m_hasCaster; }
private:
    bool m_hasCaster = false;
    // bgfx::TextureHandle     m_shadowMap; bgfx::FrameBufferHandle m_shadowFB;
    // bgfx::ProgramHandle     m_program;   float m_lightView[16], m_lightProj[16];
};
