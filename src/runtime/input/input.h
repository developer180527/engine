#pragma once
#include "runtime/input/input_system.h"
#include "runtime/input/input_map.h"

// ── Input namespace ────────────────────────────────────────────────────────
// Thin public API over InputSystem + InputMap.
// Callable from anywhere — plugins, scripts, gameplay components.
//
// Raw key API:     Input::isKeyDown(Key::W)
// Semantic API:    Input::isActionPressed("Jump")
//                  Input::getAxis("MoveForward")
namespace Input {

// ── Raw key ────────────────────────────────────────────────────────────────
inline bool isKeyDown    (Key k) { return InputSystem::get().isKeyDown    ((int)k); }
inline bool isKeyPressed (Key k) { return InputSystem::get().isKeyPressed ((int)k); }
inline bool isKeyReleased(Key k) { return InputSystem::get().isKeyReleased((int)k); }

// ── Raw mouse ──────────────────────────────────────────────────────────────
inline bool isMouseDown    (MouseButton b) { return InputSystem::get().isMouseDown    ((int)b); }
inline bool isMousePressed (MouseButton b) { return InputSystem::get().isMousePressed ((int)b); }
inline bool isMouseReleased(MouseButton b) { return InputSystem::get().isMouseReleased((int)b); }

inline float mouseDeltaX()  { return InputSystem::get().mouseDeltaX(); }
inline float mouseDeltaY()  { return InputSystem::get().mouseDeltaY(); }
inline float scrollDeltaY() { return InputSystem::get().scrollDeltaY(); }
inline float cursorX()      { return InputSystem::get().cursorX(); }
inline float cursorY()      { return InputSystem::get().cursorY(); }

// ── Semantic actions + axes ────────────────────────────────────────────────
// StringID overloads — hash computed at call site, O(1) lookup
inline bool  isActionDown    (StringID a) { return InputMap::get().isActionDown    (a); }
inline bool  isActionPressed (StringID a) { return InputMap::get().isActionPressed (a); }
inline bool  isActionReleased(StringID a) { return InputMap::get().isActionReleased(a); }
inline float getAxis         (StringID a) { return InputMap::get().getAxis         (a); }
// const char* overloads — implicit StringID construction, same cost
inline bool  isActionDown    (const char* a) { return isActionDown    (StringID{a}); }
inline bool  isActionPressed (const char* a) { return isActionPressed (StringID{a}); }
inline bool  isActionReleased(const char* a) { return isActionReleased(StringID{a}); }
inline float getAxis         (const char* a) { return getAxis         (StringID{a}); }

// ── Text input ─────────────────────────────────────────────────────────────
// UTF-32 codepoints entered this frame — for text fields, chat etc.
inline const std::vector<uint32_t>& textInput() {
    return InputSystem::get().textInput();
}

} // namespace Input
