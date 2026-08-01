#pragma once
#include "render/passes/i_render_pass.h"
// ── SkyPass ──────────────────────────────────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT DOES (today just the background-view clear — to be owned here):
//   Establishes what sits behind opaque geometry. MVP: a clear color.
//
// EXTRACTION CHECKLIST:
//   [ ] take ownership of the background clear currently on the bg view
//   [ ] execute(): clear / draw the background
//
// FUTURE: skybox cubemap, gradient, or analytic atmosphere drawn at far depth
//   with depth-test so it doesn't overdraw opaque pixels.
class SkyPass final : public IRenderPass {
public:
    const char* name() const override { return "SkyPass"; }
    void onAttach(RenderContext& ctx) override;   // TODO
    void onDetach() override;                      // TODO
    void execute(PassContext& fc) override;        // TODO: clear / draw sky
};
