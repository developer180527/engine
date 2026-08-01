#pragma once
#include "render/passes/i_render_pass.h"
// ── ResolvePass ──────────────────────────────────────────────────────────────
// SCAFFOLD — see docs/architecture/renderer-architecture.md.
//
// WHAT IT DOES (today: the resolve-view MSAA blit — to be owned here):
//   Resolves the (optionally multisampled) scene color into the final target
//   texture the editor/game displays. The terminal pass of the frame.
//
// EXTRACTION CHECKLIST:
//   [ ] take ownership of the resolve/blit currently on the resolve view
//   [ ] execute(): blit fc.sceneColor -> fc.target
//
// FUTURE: if PostPass is enabled, composite its output instead of raw scene color.
class ResolvePass final : public IRenderPass {
public:
    const char* name() const override { return "ResolvePass"; }
    void execute(PassContext& fc) override;        // TODO: resolve/blit to target
};
