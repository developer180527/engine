#pragma once
#include "render/passes/i_render_pass.h"
// ── OpaquePass ───────────────────────────────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT DOES (today: the main body of ForwardPipeline::render — to be MOVED here):
//   The lit forward submit of all OPAQUE geometry. Packs the light array into
//   uniforms, binds base-color/normal/shadow maps, samples the shadow map that
//   ShadowPass published, and submits each item into the scene view.
//
// EXTRACTION CHECKLIST:
//   [ ] move the per-item bind()+submit loop + u_lights packing here
//   [ ] onAttach(): own the forward program + material/light uniforms
//   [ ] execute(): read fc.shadowMap/shadowMatrix/shadowParams from ShadowPass
//   [ ] consume RenderWorld's opaque bucket (already culled + sorted)
//
// FUTURE:
//   - Frustum cull + front-to-back sort done in extraction (RenderWorld), not here.
//   - MAX_LIGHTS=16 is a hard cap; excess visible lights are dropped silently.
//     Sort lights by influence so the cap keeps the important ones; move to
//     Forward+/clustered when scenes go many-light (hundreds).
class OpaquePass final : public IRenderPass {
public:
    const char* name() const override { return "OpaquePass"; }
    void onAttach(RenderContext& ctx) override;   // TODO
    void onDetach() override;                      // TODO
    void execute(PassContext& fc) override;        // TODO: pack lights + submit
};
