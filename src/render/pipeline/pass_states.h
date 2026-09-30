#pragma once
// ── pass_states — the render state each pass draws with, built in ONE place ───
//
// WO-032. The opaque pass used `BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW`, and
// DEFAULT already carries BGFX_STATE_CULL_CW, so BOTH cull bits were set. bgfx
// decodes that as cull mode 3: Metal maps it to "none" (nothing was culled on
// macOS; the depth test hid it), and D3D11 and Vulkan index a three-entry
// table past its end, which is undefined behaviour. A cull bit is now REPLACED,
// never OR'd onto a state that may already carry one.
//
// ── Why the back-face bit is CULL_CCW ───────────────────────────────────────
// Meshes are counter-clockwise-front (glTF, Assimp, ImportedScene). But every
// camera in the engine (PrimaryCameraFinder, the editor fly camera, the shadow
// pass) builds its view with bx::mtxLookAt's DEFAULT, LEFT-handed convention
// over a right-handed world. That mirrors the image left-to-right: world +X
// lands on the left of the screen (WO-033). So a front face reaches the screen
// clockwise, and removing BACK faces means culling counter-clockwise ones.
// cull_mode_test derives this from the real camera path, so the day WO-033
// makes the projection right-handed, the test fails until this flips.
#include <bgfx/defines.h>
#include <cstdint>

namespace passstate {

inline constexpr uint64_t kCullBackFaces = BGFX_STATE_CULL_CCW;   // see above

// Exactly zero or one cull bit. Both set is cull mode 3 (see above).
constexpr bool validCull(uint64_t state) {
    return (state & BGFX_STATE_CULL_MASK) != BGFX_STATE_CULL_MASK;
}
// `state` with its cull mode replaced by `cull` (0 = no culling).
constexpr uint64_t withCull(uint64_t state, uint64_t cull) {
    return (state & ~BGFX_STATE_CULL_MASK) | cull;
}

// Opaque geometry. Double-sided meshes and materials cull nothing.
constexpr uint64_t opaque(bool doubleSided) {
    return withCull(BGFX_STATE_DEFAULT, doubleSided ? 0 : kCullBackFaces);
}

// Shadow casters write depth only. The light's view is built with the same
// left-handed lookAt, so the same bit removes the same (back) faces. This is NOT
// the "cull front faces into the shadow map" acne trick; if that is ever wanted,
// it is kCullBackFaces's opposite, and it should say so here.
constexpr uint64_t shadowCaster() {
    return withCull(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS, kCullBackFaces);
}

static_assert(validCull(opaque(false)) && validCull(opaque(true)) && validCull(shadowCaster()),
              "a pass state carries both cull bits (cull mode 3)");
static_assert(!validCull(BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW),
              "validCull must reject the WO-032 state");

}  // namespace passstate
