#pragma once
// ── ui_input — what a GUI toolkit needs from the window, backend-free ───────
//
// Gameplay input goes through the HID stack and InputManager, which deal in
// devices, actions and ticks. A GUI needs something else: the pointer in
// window coordinates, scroll in the unit the device reported, keys with their
// auto-repeat, typed TEXT (which is not keys — dead keys, IME, AltGr), the
// IME's composition, focus loss, plus a few services back to the OS
// (clipboard, cursor shape, where the IME popup goes).
//
// Before this, only ImGui had any of that, through ImGui's own GLFW/SDL
// backends — so every other GUI (libgui, a Qt or Rust tool) had to talk to
// GLFW or SDL directly, and a port to a new OS meant porting each GUI's
// backend. Now the IPlatform implementation translates its native events into
// these ONCE, and any GUI consumes them. A new OS implements IPlatform; the
// GUIs come along unchanged.
//
// TOOLING input only, like wsi::. Engine systems and kits bind to actions.
#include "runtime/input/input_event.h"   // Key

#include <cstdint>
#include <string>

namespace uiin {

// Physical modifier state. The shortcut layer maps these onto "Primary" and
// friends per keyboard convention; nothing here decides what Cmd means.
struct Modifiers {
    bool shift = false, ctrl = false, alt = false, super = false;
    bool operator==(const Modifiers&) const = default;
};

// What the device reported, UNCONVERTED: a trackpad's pixel deltas and a
// wheel's detents want different handling, and only the GUI knows which.
enum class WheelUnit : uint8_t { Pixel, Line, Page };

enum class PointerButton : uint8_t { Primary, Secondary, Middle, Back, Forward };

enum class EventKind : uint8_t {
    PointerMoved,    // x, y
    PointerButton,   // button, pressed, x, y, mods
    PointerLeft,     // the pointer left the window
    Wheel,           // x, y = delta; unit
    Key,             // key, pressed, repeat, mods
    Text,            // text: UTF-8 the user TYPED (after layout, dead keys, IME commit)
    ImePreedit,      // text: the composition in progress; cursor = byte offset in it
    FocusLost,       // the window stopped receiving keys
};

struct Event {
    EventKind     kind    = EventKind::PointerMoved;
    float         x = 0.0f, y = 0.0f;   // logical window units (points), top-left origin
    PointerButton button  = PointerButton::Primary;
    bool          pressed = false;
    bool          repeat  = false;
    WheelUnit     unit    = WheelUnit::Line;
    Key           key     = Key::Unknown;
    Modifiers     mods;
    std::string   text;
    uint32_t      cursor  = 0;
};

// The OS pointer shapes a GUI asks for. Backends map what they have and fall
// back to Arrow for the rest.
enum class Cursor : uint8_t {
    Arrow, Text, Hand, Crosshair, Move, NotAllowed, Wait,
    ResizeEW, ResizeNS, ResizeNWSE, ResizeNESW,
    Hidden,
};

// Which platform convention the keyboard follows. This is a property of the
// PLATFORM (where Cmd sits, what Redo is), not of the machine the engine was
// compiled on — an exotic target reports whichever it follows.
enum class KeyboardConvention : uint8_t {
    Apple,   // Cmd is the command modifier; Ctrl is for the terminal and friends
    Pc,      // Ctrl is the command modifier (Windows, Linux, most others)
};

// The compile-time best guess, for platforms that do not say.
constexpr KeyboardConvention defaultKeyboardConvention() {
#if defined(__APPLE__)
    return KeyboardConvention::Apple;
#else  // any OS: Ctrl-based shortcuts are the convention everywhere but Apple
    return KeyboardConvention::Pc;
#endif
}

}  // namespace uiin
