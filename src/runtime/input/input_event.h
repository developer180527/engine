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
    // Modifiers
    LeftShift = 340, LeftCtrl  = 341, LeftAlt  = 342, LeftSuper  = 343,
    RightShift= 344, RightCtrl = 345, RightAlt = 346, RightSuper = 347,
};

enum class MouseButton : int {
    Left   = 0,
    Right  = 1,
    Middle = 2,
};
