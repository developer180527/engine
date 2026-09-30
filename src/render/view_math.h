#pragma once
// ── view_math — the ONE place a view or projection matrix is built (WO-033) ───
//
// The world is right-handed: glTF, ImportedScene, and a camera that looks down
// its local −Z with +Y up. Every view used to be built with bx::mtxLookAt's
// DEFAULT handedness, which is LEFT, and every projection with bx's left-handed
// default. A left-handed view over a right-handed world is a MIRROR: world +X
// landed on the LEFT of the screen. The engine grew compensations instead of
// noticing (the editor's "move right" key moved along −right; the pipeline
// culled CCW to remove back faces).
//
// Every camera calls these, and audit rule CAM-01 forbids calling
// bx::mtxLookAt / mtxProj / mtxOrtho anywhere else, so the four cameras (game,
// editor, shadow light, and anything added later) cannot disagree again.
//
// Matrices use the bgfx/bx memory layout (translation in m[12..14]), as before.
#include <bx/math.h>

namespace viewmath {

inline constexpr bx::Handedness::Enum kHandedness = bx::Handedness::Right;

// A camera at `eye` looking at `at`. world +X to the camera's right lands on
// the RIGHT of the screen; tests/cull_mode_test.cpp pins it.
inline void lookAt(float out[16], const bx::Vec3& eye, const bx::Vec3& at, const bx::Vec3& up) {
    bx::mtxLookAt(out, eye, at, up, kHandedness);
}

// fovY in degrees. `homogeneousDepth`: bgfx::getCaps()->homogeneousDepth
// (true = −1..1 depth, OpenGL; false = 0..1, Metal/D3D/Vulkan).
inline void perspective(float out[16], float fovYDeg, float aspect, float nearZ, float farZ,
                        bool homogeneousDepth) {
    bx::mtxProj(out, fovYDeg, aspect, nearZ, farZ, homogeneousDepth, kHandedness);
}

inline void orthographic(float out[16], float left, float right, float bottom, float top,
                         float nearZ, float farZ, bool homogeneousDepth) {
    bx::mtxOrtho(out, left, right, bottom, top, nearZ, farZ, 0.0f, homogeneousDepth, kHandedness);
}

}  // namespace viewmath
