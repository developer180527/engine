#pragma once
// ── Free-fly editor camera, without a GUI ────────────────────────────────────
// The Scene View's camera and how it moves: look by a pointer delta while the
// right button is held, move with WASD/QE, Shift for fast. What a front end
// supplies is only the INPUT (FlyInput), gathered however that GUI gathers it
// — ImGui's editor polls wsi:: (editor_camera.h), the libgui front end reads
// its UI events. The movement itself is one function, so both editors fly
// the same.
#include <bx/math.h>

#include <algorithm>
#include <cmath>

// Owned by the editor, never serialized into scenes. The game camera is a
// scene component; this is developer-only.
struct EditorCamera {
    bx::Vec3 position { 0.0f, 6.0f, 18.0f };
    float    yaw   = 0.0f;
    float    pitch = 0.0f;

    bx::Vec3 forward() const {
        return {
             std::sin(yaw) * std::cos(pitch),
             std::sin(pitch),
            -std::cos(yaw) * std::cos(pitch)
        };
    }
    bx::Vec3 right() const { return { std::cos(yaw), 0.0f, std::sin(yaw) }; }
    bx::Vec3 up()    const { return { 0.0f, 1.0f, 0.0f }; }

    void getViewMatrix(float out[16]) const {
        bx::mtxLookAt(out, position, bx::add(position, forward()), up());
    }
};

struct FlyInput {
    float lookDx = 0.0f, lookDy = 0.0f;   // pointer delta while looking, px
    bool  forward = false, back = false, left = false, right = false, up = false, down = false;
    bool  fast = false;
};

inline void applyFly(EditorCamera& cam, const FlyInput& in, float dt) {
    constexpr float kSensitivity = 0.0025f;
    // Clamp the delta to 200 px — one bad frame (a Bluetooth hiccup, a focus
    // change) must not spin the camera.
    constexpr float kMaxDelta = 200.0f;
    cam.yaw   -= std::clamp(in.lookDx, -kMaxDelta, kMaxDelta) * kSensitivity;
    cam.pitch -= std::clamp(in.lookDy, -kMaxDelta, kMaxDelta) * kSensitivity;
    const float kLimit = bx::kPiHalf - 0.01f;
    cam.pitch = std::clamp(cam.pitch, -kLimit, kLimit);

    const float step = (in.fast ? 20.0f : 5.0f) * dt;
    const bx::Vec3 fwd = cam.forward();
    const bx::Vec3 rt  = cam.right();
    const bx::Vec3 wup = {0.0f, 1.0f, 0.0f};
    if (in.forward) cam.position = bx::add(cam.position, bx::mul(fwd,  step));
    if (in.back)    cam.position = bx::add(cam.position, bx::mul(fwd, -step));
    if (in.right)   cam.position = bx::add(cam.position, bx::mul(rt,  -step));
    if (in.left)    cam.position = bx::add(cam.position, bx::mul(rt,   step));
    if (in.up)      cam.position = bx::add(cam.position, bx::mul(wup,  step));
    if (in.down)    cam.position = bx::add(cam.position, bx::mul(wup, -step));
}
