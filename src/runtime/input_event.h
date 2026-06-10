#pragma once
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstdint>

// ── Key enum ───────────────────────────────────────────────────────────────
// Mirrors GLFW key codes directly — zero-cost cast, no lookup table.
enum class Key : int {
    Unknown   = GLFW_KEY_UNKNOWN,
    Space     = GLFW_KEY_SPACE,
    Enter     = GLFW_KEY_ENTER,
    Escape    = GLFW_KEY_ESCAPE,
    Tab       = GLFW_KEY_TAB,
    Backspace = GLFW_KEY_BACKSPACE,
    Delete    = GLFW_KEY_DELETE,
    // Arrows
    Right = GLFW_KEY_RIGHT, Left = GLFW_KEY_LEFT,
    Up    = GLFW_KEY_UP,    Down = GLFW_KEY_DOWN,
    // Letters
    A=GLFW_KEY_A, B=GLFW_KEY_B, C=GLFW_KEY_C, D=GLFW_KEY_D,
    E=GLFW_KEY_E, F=GLFW_KEY_F, G=GLFW_KEY_G, H=GLFW_KEY_H,
    I=GLFW_KEY_I, J=GLFW_KEY_J, K=GLFW_KEY_K, L=GLFW_KEY_L,
    M=GLFW_KEY_M, N=GLFW_KEY_N, O=GLFW_KEY_O, P=GLFW_KEY_P,
    Q=GLFW_KEY_Q, R=GLFW_KEY_R, S=GLFW_KEY_S, T=GLFW_KEY_T,
    U=GLFW_KEY_U, V=GLFW_KEY_V, W=GLFW_KEY_W, X=GLFW_KEY_X,
    Y=GLFW_KEY_Y, Z=GLFW_KEY_Z,
    // Numbers
    Num0=GLFW_KEY_0, Num1=GLFW_KEY_1, Num2=GLFW_KEY_2,
    Num3=GLFW_KEY_3, Num4=GLFW_KEY_4, Num5=GLFW_KEY_5,
    Num6=GLFW_KEY_6, Num7=GLFW_KEY_7, Num8=GLFW_KEY_8,
    Num9=GLFW_KEY_9,
    // Function
    F1=GLFW_KEY_F1,   F2=GLFW_KEY_F2,   F3=GLFW_KEY_F3,
    F4=GLFW_KEY_F4,   F5=GLFW_KEY_F5,   F6=GLFW_KEY_F6,
    F7=GLFW_KEY_F7,   F8=GLFW_KEY_F8,   F9=GLFW_KEY_F9,
    F10=GLFW_KEY_F10, F11=GLFW_KEY_F11, F12=GLFW_KEY_F12,
    // Modifiers
    LeftShift  = GLFW_KEY_LEFT_SHIFT,   RightShift  = GLFW_KEY_RIGHT_SHIFT,
    LeftCtrl   = GLFW_KEY_LEFT_CONTROL, RightCtrl   = GLFW_KEY_RIGHT_CONTROL,
    LeftAlt    = GLFW_KEY_LEFT_ALT,     RightAlt    = GLFW_KEY_RIGHT_ALT,
    LeftSuper  = GLFW_KEY_LEFT_SUPER,   RightSuper  = GLFW_KEY_RIGHT_SUPER,
};

enum class MouseButton : int {
    Left   = GLFW_MOUSE_BUTTON_LEFT,
    Right  = GLFW_MOUSE_BUTTON_RIGHT,
    Middle = GLFW_MOUSE_BUTTON_MIDDLE,
};

// ── InputEvent ─────────────────────────────────────────────────────────────
// Every OS input event is captured into this struct and pushed onto
// the queue. Processed at frame start — never dropped between ticks.
enum class InputEventType {
    KeyPress, KeyRelease,
    MouseButtonPress, MouseButtonRelease,
    MouseMove, Scroll,
    TextInput,
};

struct InputEvent {
    InputEventType type      = InputEventType::KeyPress;
    int            key       = 0;       // GLFW_KEY_* for key events
    int            button    = 0;       // GLFW_MOUSE_BUTTON_* for mouse events
    float          x         = 0.0f;   // cursor delta x / scroll x
    float          y         = 0.0f;   // cursor delta y / scroll y
    uint32_t       codepoint = 0;      // UTF-32 for TextInput events
};
