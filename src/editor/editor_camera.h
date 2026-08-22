#pragma once
#include <bx/math.h>
#include "runtime/platform/window_ops.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// Free-fly editor camera. Owned by EditorApp, never serialized into scenes.
// The game camera will be a scene component; this is developer-only.
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

struct EditorInput {
    bool   rightMouseHeld = false;
    double lastMouseX     = 0.0;
    double lastMouseY     = 0.0;
};

inline void updateEditorCamera(EditorCamera& cam, EditorInput& inp,
                                wsi::WindowHandle window, float dt,
                                bool sceneHovered = true) {
    ImGuiIO& io = ImGui::GetIO();
    const bool typing       = io.WantTextInput;
    // Scene View IS an ImGui window so WantCaptureMouse is always true there.
    // Use sceneHovered (set by panel) instead.
    const bool rightDownNow =
        wsi::isMouseButtonDown(window, MouseButton::Right) && sceneHovered;

    if (rightDownNow && !inp.rightMouseHeld) {
        wsi::setCursorMode(window, CursorMode::Captured);
        wsi::cursorPos(window, inp.lastMouseX, inp.lastMouseY);
        inp.rightMouseHeld = true;
    } else if (!rightDownNow && inp.rightMouseHeld) {
        wsi::setCursorMode(window, CursorMode::Normal);
        inp.rightMouseHeld = false;
    }

    if (inp.rightMouseHeld) {
        double mx, my;
        wsi::cursorPos(window, mx, my);
        const float dx = float(mx - inp.lastMouseX);
        const float dy = float(my - inp.lastMouseY);
        inp.lastMouseX = mx;
        inp.lastMouseY = my;

        constexpr float kSensitivity = 0.0025f;
        // Clamp delta to 200px max — prevents a single bad frame from a
        // Bluetooth hiccup or focus-loss event spinning the camera wildly.
        constexpr float kMaxDelta = 200.0f;
        const float cdx = std::clamp(dx, -kMaxDelta, kMaxDelta);
        const float cdy = std::clamp(dy, -kMaxDelta, kMaxDelta);
        cam.yaw   -= cdx * kSensitivity;
        cam.pitch -= cdy * kSensitivity;
        const float kLimit = bx::kPiHalf - 0.01f;
        cam.pitch = std::clamp(cam.pitch, -kLimit, kLimit);
    }

    if (typing || !sceneHovered) return;

    const float speed = (wsi::isKeyDown(window, Key::LeftShift) ||
                         wsi::isKeyDown(window, Key::RightShift))
                        ? 20.0f : 5.0f;
    const float step = speed * dt;
    const bx::Vec3 fwd = cam.forward();
    const bx::Vec3 rt  = cam.right();

    if (wsi::isKeyDown(window, Key::W)) cam.position = bx::add(cam.position, bx::mul(fwd,  step));
    if (wsi::isKeyDown(window, Key::S)) cam.position = bx::add(cam.position, bx::mul(fwd, -step));
    if (wsi::isKeyDown(window, Key::D)) cam.position = bx::add(cam.position, bx::mul(rt,  -step));
    if (wsi::isKeyDown(window, Key::A)) cam.position = bx::add(cam.position, bx::mul(rt,   step));
    const bx::Vec3 wup = {0.0f, 1.0f, 0.0f};
    if (wsi::isKeyDown(window, Key::E)) cam.position = bx::add(cam.position, bx::mul(wup,  step));
    if (wsi::isKeyDown(window, Key::Q)) cam.position = bx::add(cam.position, bx::mul(wup, -step));
}
