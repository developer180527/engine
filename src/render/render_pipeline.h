#pragma once
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
};
