#pragma once
#include <bx/math.h>
#include "runtime/platform/window_ops.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

// Free-fly editor camera: EditorCamera and the movement are GUI-free
// (fly_camera.h); this file gathers the ImGui editor's input for it.
#include "editor/fly_camera.h"

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

    FlyInput in;
    if (inp.rightMouseHeld) {
        double mx, my;
        wsi::cursorPos(window, mx, my);
        in.lookDx = float(mx - inp.lastMouseX);
        in.lookDy = float(my - inp.lastMouseY);
        inp.lastMouseX = mx;
        inp.lastMouseY = my;
    }
    // Looking still applies while typing or away from the panel (the drag
    // began in it); moving does not.
    if (!typing && sceneHovered) {
        in.fast    = wsi::isKeyDown(window, Key::LeftShift) || wsi::isKeyDown(window, Key::RightShift);
        in.forward = wsi::isKeyDown(window, Key::W);
        in.back    = wsi::isKeyDown(window, Key::S);
        in.right   = wsi::isKeyDown(window, Key::D);
        in.left    = wsi::isKeyDown(window, Key::A);
        in.up      = wsi::isKeyDown(window, Key::E);
        in.down    = wsi::isKeyDown(window, Key::Q);
    }
    applyFly(cam, in, dt);
}
