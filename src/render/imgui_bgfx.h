#pragma once

#include <bgfx/bgfx.h>

struct GLFWwindow;

// Initialize ImGui with the bgfx renderer and GLFW input backend.
// Must be called after bgfx::init() and after glfwCreateWindow().
void imguiInit(GLFWwindow* window, float fontSize = 16.0f);

// Tear down ImGui and release all bgfx resources it owns.
void imguiShutdown();

// Begin a new ImGui frame. Call this once per frame before any ImGui:: calls.
void imguiNewFrame();

// Render the accumulated ImGui draw data to the given bgfx view.
// Call this after all ImGui:: calls for the frame.
void imguiRender(bgfx::ViewId viewId = 255);
