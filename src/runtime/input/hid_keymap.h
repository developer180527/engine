#pragma once
// ── HID usage <-> engine translations ───────────────────────────────────────
// The hid module speaks HID usages (keyboard page 0x07). These helpers are
// the ONLY place the engine translates: GLFW keycodes -> usages (fallback
// WindowSource) and binding-spec key names -> usages (input.json).
#include <cstdint>
#include <cstring>

namespace input {

// GLFW keycode -> HID keyboard usage. 0 = unmapped (event dropped).
inline uint16_t usageFromGlfw(int key) {
    if (key >= 65 && key <= 90)  return (uint16_t)(0x04 + (key - 65));   // A..Z
    if (key >= 49 && key <= 57)  return (uint16_t)(0x1E + (key - 49));   // 1..9
    if (key == 48)               return 0x27;                            // 0
    if (key >= 290 && key <= 301) return (uint16_t)(0x3A + (key - 290)); // F1..F12
    switch (key) {
        case 32:  return 0x2C;   // Space
        case 256: return 0x29;   // Escape
        case 257: return 0x28;   // Enter
        case 258: return 0x2B;   // Tab
        case 259: return 0x2A;   // Backspace
        case 262: return 0x4F;   // Right
        case 263: return 0x50;   // Left
        case 264: return 0x51;   // Down
        case 265: return 0x52;   // Up
        case 340: return 0xE1;   // LShift
        case 341: return 0xE0;   // LCtrl
        case 342: return 0xE2;   // LAlt
        case 343: return 0xE3;   // LSuper
        case 344: return 0xE5;   // RShift
        case 345: return 0xE4;   // RCtrl
        default:  return 0;
    }
}

// Binding-spec key name -> HID usage ("W", "3", "Space", "LShift"…).
inline uint16_t usageFromName(const char* n) {
    if (!n || !n[0]) return 0;
    if (!n[1]) {   // single char
        const char c = n[0];
        if (c >= 'A' && c <= 'Z') return (uint16_t)(0x04 + (c - 'A'));
        if (c >= 'a' && c <= 'z') return (uint16_t)(0x04 + (c - 'a'));
        if (c >= '1' && c <= '9') return (uint16_t)(0x1E + (c - '1'));
        if (c == '0')             return 0x27;
        return 0;
    }
    struct Named { const char* name; uint16_t usage; };
    static constexpr Named kNamed[] = {
        {"Space",0x2C},{"Enter",0x28},{"Escape",0x29},{"Tab",0x2B},
        {"Backspace",0x2A},{"Right",0x4F},{"Left",0x50},{"Down",0x51},
        {"Up",0x52},{"LShift",0xE1},{"LCtrl",0xE0},{"LAlt",0xE2},
        {"LSuper",0xE3},{"RShift",0xE5},{"RCtrl",0xE4},
    };
    for (const Named& k : kNamed)
        if (!std::strcmp(n, k.name)) return k.usage;
    if (n[0] == 'F') {   // F1..F12
        const int f = std::atoi(n + 1);
        if (f >= 1 && f <= 12) return (uint16_t)(0x3A + (f - 1));
    }
    return 0;
}

} // namespace input
