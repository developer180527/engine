// ── headless_include_probe — runtime.h must not reach a graphics API ────────
//
// This file's CONTENT is trivial. Its build settings are the test: the CMake
// target that compiles it is given every engine include directory EXCEPT
// bgfx's. If anything reachable from `runtime/runtime.h` or the pipeline seam
// includes <bgfx/bgfx.h>, this fails to compile with "file not found".
//
// WHY A PROBE AND NOT A GREP. scripts/check_gpu_seam.py greps for direct
// includes, which is the right tool for "did somebody re-couple a loader". It
// is the wrong tool here, because the coupling that mattered was TRANSITIVE:
// runtime.h never named bgfx: it included render/renderer.h, which declared
// bgfx handle members, and before that render/mesh.h did, and before that
// render/vertex.h did through Vertex::layout(). Every one of those is invisible
// to a grep of runtime.h, and each was found only by walking `-H` output by
// hand. A compiler with bgfx off the path finds all of them at once, forever.
//
// WHAT THIS DOES NOT PROVE: that engine_runtime LINKS without bgfx. It does
// not — the runtime still calls Renderer methods whose definitions live in TUs
// that use bgfx, so the symbols are required even in a build that never creates
// a device. Closing that needs a null renderer implementation, which is a
// design decision recorded in docs/rhi/phases.md G1c, not an oversight here.
//
// bx IS ALLOWED. It is bgfx's MATH library — Vec3, mtxMul — and 18 files depend
// on it for maths alone. Whether to keep it is a separate question on its own
// schedule (docs/rhi/evidence-coupling.md 2.1), and sweeping it into G1 would
// have made this change four times larger for no headless benefit.
#include "runtime/runtime.h"

// The pipeline-authoring seam (G1b): a third-party IRenderPipeline must be
// declarable without a graphics API in scope, or "swap the whole renderer by
// assigning a different IRenderPipeline" is not a claim anybody outside this
// repo can act on.
#include "render/render_pipeline.h"
#include "render/render_context.h"

namespace {
struct ThirdPartyPipeline final : IRenderPipeline {
    const char* name() const override { return "third-party"; }
    void render(const RenderView&, RenderContext&) override {}
};
}  // namespace

int main() {
    // Never run for effect — the compile IS the assertion. Instantiated so the
    // class is not merely parsed: an ODR-used override forces the vtable, which
    // is what a real third-party pipeline would need.
    ThirdPartyPipeline p;
    (void)p;
    return 0;
}
