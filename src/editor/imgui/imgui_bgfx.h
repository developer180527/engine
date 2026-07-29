#pragma once

#include <bgfx/bgfx.h>


// Initialize ImGui with the bgfx renderer and the platform input backend.
// Must be called after bgfx::init() and after the window exists.
// `window` is the platform backend's window handle (opaque here).
void imguiInit(void* window, float fontSize = 16.0f);

// Hand one native event (SDL_Event* on SDL3) to the ImGui platform backend.
// Wire to IPlatform::setNativeEventHook. No-op on GLFW, whose backend installs
// window callbacks instead.
void imguiProcessNativeEvent(const void* nativeEvent);

// Tear down ImGui and release all bgfx resources it owns.
void imguiShutdown();

// Begin a new ImGui frame. Call this once per frame before any ImGui:: calls.
void imguiNewFrame();

// Render the accumulated ImGui draw data to the given bgfx view.
// Call this after all ImGui:: calls for the frame.
void imguiRender(bgfx::ViewId viewId = 255);
void imguiRenderViewports();
