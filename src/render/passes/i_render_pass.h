#pragma once
// ── IRenderPass ──────────────────────────────────────────────────────────────
// SCAFFOLD — not yet wired into the build. See docs/architecture/renderer-architecture.md.
//
// A render pass is ONE self-contained stage of a frame: it claims a bgfx view,
// reads prepared render data + any resources earlier passes produced, and
// records draw calls. Passes are ordered and run by PassListPipeline.
//
// Lifecycle:
//   onAttach(ctx) - once on attach: create GPU resources (programs, FBs, uniforms)
//   setup(fc)     - per frame, before any execute(): claim view id(s), compute
//                   matrices, decide whether the pass runs this frame
//   execute(fc)   - per frame: set view state + submit draws
//   onDetach()    - once on teardown: destroy GPU resources
//
// Passes NEVER touch the ECS world or registries directly — they consume the
// already-extracted RenderWorld via PassContext. That boundary is the point.
struct RenderContext;   // render/render_context.h
struct PassContext;     // render/passes/pass_context.h

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual const char* name() const = 0;          // for logs / profiler labels

    virtual void onAttach(RenderContext& ctx) {}   // create resources
    virtual void onDetach() {}                      // destroy resources

    // setup() runs for ALL passes first, then execute() for all, so a pass can
    // publish a resource in setup() that a later pass reads in execute().
    virtual void setup(PassContext& fc) {}
    virtual void execute(PassContext& fc) = 0;

    virtual bool enabled() const { return true; }   // cheap per-frame skip
};
