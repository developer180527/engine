#pragma once
#include "render/passes/i_render_pass.h"
// ── PostPass (NOT BUILT — future) ────────────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT WILL DO:
//   Full-screen post over the scene color: tonemap + exposure, then optional
//   bloom. Reads fc.sceneColor, writes the post result.
//
// IMPORTANT — this is where the COLOR-SPACE fix lands:
//   Lighting currently accumulates in GAMMA space (deliberate, zero-regression).
//   When PostPass exists, move lighting to LINEAR and do linear->sRGB / tonemap
//   HERE as the single conversion point.
//
// WHAT HAS TO BE MADE FIRST:
//   [ ] a fullscreen-triangle program + tonemap shader
//   [ ] an HDR (RGBA16F) scene target so values can exceed 1.0
class PostPass final : public IRenderPass {
public:
    const char* name() const override { return "PostPass"; }
    void onAttach(RenderContext& ctx) override;   // TODO
    void onDetach() override;                      // TODO
    void execute(PassContext& fc) override;        // TODO: fullscreen tonemap
    bool enabled() const override { return false; } // off until built
};
