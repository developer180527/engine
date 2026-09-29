#pragma once
// ── sdl3_keymap — Key <-> SDL_Scancode, in ONE place ────────────────────────
// The engine's Key values are GLFW's numbers (input_event.h), so the GLFW
// backend can cast. SDL uses its own enumeration, so every SDL-backed input
// path must TRANSLATE — and there is more than one such path (the window input
// source and the editor's window-ops seam). Two copies of a keymap is exactly
// the thing that silently drifts, so both include this.
//
// SCANCODES, not keycodes: scancodes are physical key positions, which is what
// glfwGetKey() reports. Mapping to keycodes instead would move WASD for AZERTY
// or Dvorak users — the classic version of this bug.
#include "runtime/input/input_event.h"

#include <SDL3/SDL.h>

namespace sdl3keys {

inline SDL_Scancode toScancode(Key k) {
    switch (k) {
        case Key::Space:      return SDL_SCANCODE_SPACE;
        case Key::Escape:     return SDL_SCANCODE_ESCAPE;
        case Key::Enter:      return SDL_SCANCODE_RETURN;
        case Key::Tab:        return SDL_SCANCODE_TAB;
        case Key::Backspace:  return SDL_SCANCODE_BACKSPACE;
        case Key::Delete:     return SDL_SCANCODE_DELETE;
        case Key::Right:      return SDL_SCANCODE_RIGHT;
        case Key::Left:       return SDL_SCANCODE_LEFT;
        case Key::Down:       return SDL_SCANCODE_DOWN;
        case Key::Up:         return SDL_SCANCODE_UP;

        case Key::A: return SDL_SCANCODE_A;  case Key::B: return SDL_SCANCODE_B;
        case Key::C: return SDL_SCANCODE_C;  case Key::D: return SDL_SCANCODE_D;
        case Key::E: return SDL_SCANCODE_E;  case Key::F: return SDL_SCANCODE_F;
        case Key::G: return SDL_SCANCODE_G;  case Key::H: return SDL_SCANCODE_H;
        case Key::I: return SDL_SCANCODE_I;  case Key::J: return SDL_SCANCODE_J;
        case Key::K: return SDL_SCANCODE_K;  case Key::L: return SDL_SCANCODE_L;
        case Key::M: return SDL_SCANCODE_M;  case Key::N: return SDL_SCANCODE_N;
        case Key::O: return SDL_SCANCODE_O;  case Key::P: return SDL_SCANCODE_P;
        case Key::Q: return SDL_SCANCODE_Q;  case Key::R: return SDL_SCANCODE_R;
        case Key::S: return SDL_SCANCODE_S;  case Key::T: return SDL_SCANCODE_T;
        case Key::U: return SDL_SCANCODE_U;  case Key::V: return SDL_SCANCODE_V;
        case Key::W: return SDL_SCANCODE_W;  case Key::X: return SDL_SCANCODE_X;
        case Key::Y: return SDL_SCANCODE_Y;  case Key::Z: return SDL_SCANCODE_Z;

        // Number ROW, not the keypad.
        case Key::Num0: return SDL_SCANCODE_0;
        case Key::Num1: return SDL_SCANCODE_1;
        case Key::Num2: return SDL_SCANCODE_2;
        case Key::Num3: return SDL_SCANCODE_3;
        case Key::Num4: return SDL_SCANCODE_4;
        case Key::Num5: return SDL_SCANCODE_5;
        case Key::Num6: return SDL_SCANCODE_6;
        case Key::Num7: return SDL_SCANCODE_7;
        case Key::Num8: return SDL_SCANCODE_8;
        case Key::Num9: return SDL_SCANCODE_9;

        case Key::F1:  return SDL_SCANCODE_F1;   case Key::F2:  return SDL_SCANCODE_F2;
        case Key::F3:  return SDL_SCANCODE_F3;   case Key::F4:  return SDL_SCANCODE_F4;
        case Key::F5:  return SDL_SCANCODE_F5;   case Key::F6:  return SDL_SCANCODE_F6;
        case Key::F7:  return SDL_SCANCODE_F7;   case Key::F8:  return SDL_SCANCODE_F8;
        case Key::F9:  return SDL_SCANCODE_F9;   case Key::F10: return SDL_SCANCODE_F10;
        case Key::F11: return SDL_SCANCODE_F11;  case Key::F12: return SDL_SCANCODE_F12;

        case Key::Insert:     return SDL_SCANCODE_INSERT;
        case Key::PageUp:     return SDL_SCANCODE_PAGEUP;
        case Key::PageDown:   return SDL_SCANCODE_PAGEDOWN;
        case Key::Home:       return SDL_SCANCODE_HOME;
        case Key::End:        return SDL_SCANCODE_END;
        case Key::CapsLock:   return SDL_SCANCODE_CAPSLOCK;
        case Key::Menu:       return SDL_SCANCODE_APPLICATION;

        case Key::Apostrophe:   return SDL_SCANCODE_APOSTROPHE;
        case Key::Comma:        return SDL_SCANCODE_COMMA;
        case Key::Minus:        return SDL_SCANCODE_MINUS;
        case Key::Period:       return SDL_SCANCODE_PERIOD;
        case Key::Slash:        return SDL_SCANCODE_SLASH;
        case Key::Semicolon:    return SDL_SCANCODE_SEMICOLON;
        case Key::Equal:        return SDL_SCANCODE_EQUALS;
        case Key::LeftBracket:  return SDL_SCANCODE_LEFTBRACKET;
        case Key::Backslash:    return SDL_SCANCODE_BACKSLASH;
        case Key::RightBracket: return SDL_SCANCODE_RIGHTBRACKET;
        case Key::GraveAccent:  return SDL_SCANCODE_GRAVE;

        case Key::Kp0: return SDL_SCANCODE_KP_0;  case Key::Kp1: return SDL_SCANCODE_KP_1;
        case Key::Kp2: return SDL_SCANCODE_KP_2;  case Key::Kp3: return SDL_SCANCODE_KP_3;
        case Key::Kp4: return SDL_SCANCODE_KP_4;  case Key::Kp5: return SDL_SCANCODE_KP_5;
        case Key::Kp6: return SDL_SCANCODE_KP_6;  case Key::Kp7: return SDL_SCANCODE_KP_7;
        case Key::Kp8: return SDL_SCANCODE_KP_8;  case Key::Kp9: return SDL_SCANCODE_KP_9;
        case Key::KpDecimal:  return SDL_SCANCODE_KP_PERIOD;
        case Key::KpDivide:   return SDL_SCANCODE_KP_DIVIDE;
        case Key::KpMultiply: return SDL_SCANCODE_KP_MULTIPLY;
        case Key::KpSubtract: return SDL_SCANCODE_KP_MINUS;
        case Key::KpAdd:      return SDL_SCANCODE_KP_PLUS;
        case Key::KpEnter:    return SDL_SCANCODE_KP_ENTER;
        case Key::KpEqual:    return SDL_SCANCODE_KP_EQUALS;

        case Key::LeftShift:  return SDL_SCANCODE_LSHIFT;
        case Key::LeftCtrl:   return SDL_SCANCODE_LCTRL;
        case Key::LeftAlt:    return SDL_SCANCODE_LALT;
        case Key::LeftSuper:  return SDL_SCANCODE_LGUI;
        case Key::RightShift: return SDL_SCANCODE_RSHIFT;
        case Key::RightCtrl:  return SDL_SCANCODE_RCTRL;
        case Key::RightAlt:   return SDL_SCANCODE_RALT;
        case Key::RightSuper: return SDL_SCANCODE_RGUI;

        case Key::Unknown:
        default:              return SDL_SCANCODE_UNKNOWN;
    }
}

// Every Key the engine defines. The SDL path cannot "poll all keycodes" the way
// the GLFW path does (its state array is indexed by SCANCODE, not by our code
// space), so it iterates this list instead. Keep in sync with Key.
inline const Key* allKeys(size_t& count) {
    static const Key kKeys[] = {
        Key::Space, Key::Escape, Key::Enter, Key::Tab, Key::Backspace,
        Key::Delete, Key::Right, Key::Left, Key::Down, Key::Up,
        Key::A, Key::B, Key::C, Key::D, Key::E, Key::F, Key::G, Key::H,
        Key::I, Key::J, Key::K, Key::L, Key::M, Key::N, Key::O, Key::P,
        Key::Q, Key::R, Key::S, Key::T, Key::U, Key::V, Key::W, Key::X,
        Key::Y, Key::Z,
        Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4,
        Key::Num5, Key::Num6, Key::Num7, Key::Num8, Key::Num9,
        Key::F1, Key::F2, Key::F3, Key::F4, Key::F5, Key::F6,
        Key::F7, Key::F8, Key::F9, Key::F10, Key::F11, Key::F12,
        Key::Insert, Key::PageUp, Key::PageDown, Key::Home, Key::End,
        Key::CapsLock, Key::Menu,
        Key::Apostrophe, Key::Comma, Key::Minus, Key::Period, Key::Slash,
        Key::Semicolon, Key::Equal, Key::LeftBracket, Key::Backslash,
        Key::RightBracket, Key::GraveAccent,
        Key::Kp0, Key::Kp1, Key::Kp2, Key::Kp3, Key::Kp4,
        Key::Kp5, Key::Kp6, Key::Kp7, Key::Kp8, Key::Kp9,
        Key::KpDecimal, Key::KpDivide, Key::KpMultiply, Key::KpSubtract,
        Key::KpAdd, Key::KpEnter, Key::KpEqual,
        Key::LeftShift, Key::LeftCtrl, Key::LeftAlt, Key::LeftSuper,
        Key::RightShift, Key::RightCtrl, Key::RightAlt, Key::RightSuper,
    };
    count = sizeof(kKeys) / sizeof(kKeys[0]);
    return kKeys;
}

// SDL scancode -> Key: the reverse of toScancode, for event-driven paths (UI
// input) that receive scancodes rather than poll by Key. Built from the same
// table, so the two directions cannot disagree.
inline Key fromScancode(SDL_Scancode sc) {
    static Key table[SDL_SCANCODE_COUNT] = {};
    static const bool built = [] {
        for (auto& k : table) k = Key::Unknown;
        size_t n = 0;
        const Key* keys = allKeys(n);
        for (size_t i = 0; i < n; ++i) {
            const SDL_Scancode s = toScancode(keys[i]);
            if (s != SDL_SCANCODE_UNKNOWN) table[s] = keys[i];
        }
        return true;
    }();
    (void)built;
    return (sc >= 0 && sc < SDL_SCANCODE_COUNT) ? table[sc] : Key::Unknown;
}

} // namespace sdl3keys
