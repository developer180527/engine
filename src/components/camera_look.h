#pragma once
#include <bx/math.h>
#include <cmath>

// ── CameraLook — where the camera is AIMED, at render rate ──────────────────
//
// Stage 4 of the command architecture: the presentation split. Authoritative
// simulation state, interpolated state, and RENDER state become three things
// rather than two.
//
// ── THE DEFECT THIS REMOVES ─────────────────────────────────────────────────
// A first-person controller latches the mouse in `onFrame` — it must, or look
// lags the frame rate and the game feels broken — and then wrote the resulting
// orientation into `Transform.rotation`. `Transform` is a hashed SimState
// component, so that is a RENDER-RATE WRITE TO SIMULATION STATE: the same
// content at 1 frame/tick and 2 frames/tick produces different world state.
// That is BUG-0053's defect class exactly (gameplay clocks at render rate),
// and it survives in the tree today only because no determinism-gate tier
// drives input, so nothing measures it.
//
// The fix is not to move the latch into the fixed step — that would make look
// lag, which is the thing the late-latch exists to prevent. It is to stop the
// look from being simulation state at all. A camera's aim is presentation: it
// decides what is drawn and nothing else reads it.
//
// So `CameraLook` is SimExempt, written at render rate, and composed by
// `PrimaryCameraFinder` when it builds the view. `Transform.rotation` is left
// untouched on a camera that has one.
//
// ── WHAT THIS DOES NOT FIX, STATED PLAINLY ──────────────────────────────────
// Yaw and pitch still ACCUMULATE at frame rate, and a controller still feeds
// yaw into its movement direction and its raycasts — which are simulation. So
// the *simulation* remains frame-rate-coupled through the player's aim; what
// is removed is the write to a hashed component, which is what made the
// coupling reach world state directly. Closing the rest needs the INPUT LAYER
// named in runtime/sim_command.h: intent sampled per tick, not per frame.
// Naming that here so this is not mistaken for a determinism fix.
struct CameraLook {
    float yaw   = 0.0f;   // radians about +Y; 0 looks along -Z
    float pitch = 0.0f;   // radians; positive is up. The writer clamps it —
                          // past +/- pi/2 the basis below flips and the view
                          // rolls, which is a controller's decision, not this
                          // struct's.
};

// ── ONE definition of the basis ─────────────────────────────────────────────
// The renderer composes this and a controller needs the same vectors for its
// movement direction and its raycasts. Two copies of this arithmetic is how
// they drift — the shot going somewhere the crosshair is not, by an amount
// nobody can reproduce. So it lives here and both call it.
//
// Roll-free by construction: `right` is derived against WORLD up rather than
// carried, so no accumulated error can tilt the horizon.
inline void cameraLookBasis(const CameraLook& l,
                            bx::Vec3& fwd, bx::Vec3& right, bx::Vec3& up) {
    fwd = bx::normalize(bx::Vec3{
        -std::sin(l.yaw) * std::cos(l.pitch),
         std::sin(l.pitch),
        -std::cos(l.yaw) * std::cos(l.pitch) });
    right = bx::normalize(bx::cross(fwd, bx::Vec3{ 0.0f, 1.0f, 0.0f }));
    up    = bx::cross(right, fwd);
}
