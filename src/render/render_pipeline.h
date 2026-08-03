#pragma once
#include "render/submit_stats.h"
#include "render/render_view.h"
#include "render/render_context.h"

// A swappable render strategy. The engine extracts a RenderView (camera +
// lights + unculled draw list) and hands it here; the impl issues all GPU work
// via bgfx. Swap the whole renderer by assigning a different IRenderPipeline.
// Default impl: ForwardPipeline.
struct IRenderPipeline {
    virtual ~IRenderPipeline() = default;
    virtual const char* name() const = 0;

    virtual void onAttach(RenderContext& /*ctx*/) {}   // create programs/uniforms/RTs
    virtual void onDetach() {}

    virtual void render(const RenderView& view, RenderContext& ctx) = 0;

    // What the last render() actually submitted. Default is all-zero, so a
    // pipeline that does not count still compiles and simply reports nothing —
    // the counters are a diagnostic, not a contract every pipeline must satisfy.
    // See render/submit_stats.h for why each counter exists.
    virtual const rdiag::SubmitStats& submitStats() const {
        static const rdiag::SubmitStats kNone{};
        return kNone;
    }
};
