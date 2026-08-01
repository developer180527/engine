#pragma once
#include "render/passes/i_render_pass.h"
// ── TransparencyPass (NOT BUILT — future) ────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT WILL DO:
//   Render alpha-blended geometry AFTER OpaquePass, back-to-front, with
//   depth-test ON but depth-write OFF, into the same scene color/depth.
//
// WHAT HAS TO BE MADE FIRST:
//   [ ] a separate "transparent" bucket in RenderWorld (extraction splits opaque
//       vs transparent by material blend mode)
//   [ ] back-to-front sort by camera distance in extraction
//   [ ] blend state (SRC_ALPHA / INV_SRC_ALPHA), no Z-write
//
// NOTE: order-independent transparency (WBOIT) is a later refinement; start with
//   sorted alpha.
class TransparencyPass final : public IRenderPass {
public:
    const char* name() const override { return "TransparencyPass"; }
    void execute(PassContext& fc) override;        // TODO: sorted alpha submit
    bool enabled() const override { return false; } // off until built
};
