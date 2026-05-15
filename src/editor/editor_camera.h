#pragma once
#include <bx/math.h>
#include <GLFW/glfw3.h>
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
                                GLFWwindow* window, float dt) {
    ImGuiIO& io = ImGui::GetIO();
    const bool typing       = io.WantTextInput;
    const bool rightDownNow = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT)
                               == GLFW_PRESS) && !io.WantCaptureMouse;

    if (rightDownNow && !inp.rightMouseHeld) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetCursorPos(window, &inp.lastMouseX, &inp.lastMouseY);
        inp.rightMouseHeld = true;
    } else if (!rightDownNow && inp.rightMouseHeld) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        inp.rightMouseHeld = false;
    }

    if (inp.rightMouseHeld) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        const float dx = float(mx - inp.lastMouseX);
        const float dy = float(my - inp.lastMouseY);
        inp.lastMouseX = mx;
        inp.lastMouseY = my;

        constexpr float kSensitivity = 0.0025f;
        cam.yaw   -= dx * kSensitivity;   // original: -= dx
        cam.pitch -= dy * kSensitivity;   // original: -= dy
        const float kLimit = bx::kPiHalf - 0.01f;
        cam.pitch = std::clamp(cam.pitch, -kLimit, kLimit);
    }

    if (typing) return;

    const float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                         glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
                        ? 20.0f : 5.0f;
    const float step = speed * dt;
    const bx::Vec3 fwd = cam.forward();
    const bx::Vec3 rt  = cam.right();

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(fwd,  step));
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(fwd, -step));
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(rt,  -step));
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(rt,   step));
    const bx::Vec3 wup = {0.0f, 1.0f, 0.0f};
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(wup,  step));
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) cam.position = bx::add(cam.position, bx::mul(wup, -step));
}
