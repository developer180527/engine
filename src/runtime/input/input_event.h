#pragma once
#include <cstdint>

// ── Key / MouseButton — engine-owned input constants ────────────────────────
// Numeric values are identical to GLFW3 key codes (a stable, documented
// mapping), but this header deliberately does NOT include GLFW: the semantic
// input layer (Key, Input::, InputMap) is backend-free public API. The
// GLFW-backed implementation (input_system.h) static_asserts the values
// match, so a backend swap is a new implementation — not a gameplay-code
// migration.
enum class Key : int {
    Unknown   = -1,
    Space     = 32,
    Escape    = 256,
    Enter     = 257,
    Tab       = 258,
    Backspace = 259,
    Delete    = 261,
    // Arrows
    Right = 262, Left = 263, Down = 264, Up = 265,
    // Letters (ASCII uppercase)
    A=65, B=66, C=67, D=68, E=69, F=70, G=71, H=72,
    I=73, J=74, K=75, L=76, M=77, N=78, O=79, P=80,
    Q=81, R=82, S=83, T=84, U=85, V=86, W=87, X=88,
    Y=89, Z=90,
    // Numbers (ASCII digits)
    Num0=48, Num1=49, Num2=50, Num3=51, Num4=52,
    Num5=53, Num6=54, Num7=55, Num8=56, Num9=57,
    // Function keys
    F1=290,  F2=291,  F3=292,  F4=293,  F5=294,  F6=295,
    F7=296,  F8=297,  F9=298,  F10=299, F11=300, F12=301,
    // Navigation and editing — what text fields and editor shortcuts need.
    Insert = 260, PageUp = 266, PageDown = 267, Home = 268, End = 269,
    CapsLock = 280, Menu = 348,
    // Punctuation, by US-layout POSITION (these are physical keys, like the
    // rest of this enum): Comma is the key right of M whatever it prints.
    Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Semicolon = 59, Equal = 61, LeftBracket = 91, Backslash = 92,
    RightBracket = 93, GraveAccent = 96,
    // Numeric keypad
    Kp0 = 320, Kp1 = 321, Kp2 = 322, Kp3 = 323, Kp4 = 324,
    Kp5 = 325, Kp6 = 326, Kp7 = 327, Kp8 = 328, Kp9 = 329,
    KpDecimal = 330, KpDivide = 331, KpMultiply = 332, KpSubtract = 333,
    KpAdd = 334, KpEnter = 335, KpEqual = 336,
    // Modifiers
    LeftShift = 340, LeftCtrl  = 341, LeftAlt  = 342, LeftSuper  = 343,
    RightShift= 344, RightCtrl = 345, RightAlt = 346, RightSuper = 347,
};

enum class MouseButton : int {
    Left   = 0,
    Right  = 1,
    Middle = 2,
};

// Bounds of the code spaces above, so state tables can be sized without a
// windowing library's constants (these were GLFW_KEY_LAST / _MOUSE_BUTTON_LAST,
// which forced every consumer of the input state to include GLFW).
// Values match GLFW's so the GLFW-backed path is byte-identical.
static constexpr int kKeyCodeMax      = 348;   // GLFW_KEY_LAST
static constexpr int kMouseButtonMax  = 7;     // GLFW_MOUSE_BUTTON_LAST
