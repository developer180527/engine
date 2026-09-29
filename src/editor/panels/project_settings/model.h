#pragma once
// ── Project Settings model — categories and input capture, without a GUI ─────
// The settings window's state (which category, what a key capture is aimed
// at, the names being typed for a new action/axis) and the one piece of logic
// in its Input category: applying a captured key to the InputMap. The rest of
// that category IS InputMap's API. The ImGui window and the libgui front end
// drive the same state.
#include "runtime/input/input_event.h"
#include "runtime/input/input_map.h"
#include "runtime/platform/window_ops.h"   // wsi::keyName

#include <string>
#include <vector>

inline const char* keyDisplayName(Key key) {
    switch (key) {
    case Key::Space:         return "Space";
    case Key::Enter:         return "Enter";
    case Key::Escape:        return "Escape";
    case Key::Tab:           return "Tab";
    case Key::Backspace:     return "Backspace";
    case Key::Delete:        return "Delete";
    case Key::Right:         return "Right";
    case Key::Left:          return "Left";
    case Key::Up:            return "Up";
    case Key::Down:          return "Down";
    case Key::LeftShift:    return "L.Shift";
    case Key::RightShift:   return "R.Shift";
    case Key::LeftCtrl:  return "L.Ctrl";
    case Key::RightCtrl: return "R.Ctrl";
    case Key::LeftAlt:      return "L.Alt";
    case Key::RightAlt:     return "R.Alt";
    case Key::LeftSuper:    return "L.Cmd";
    case Key::RightSuper:   return "R.Cmd";
    case Key::F1:  return "F1";  case Key::F2:  return "F2";
    case Key::F3:  return "F3";  case Key::F4:  return "F4";
    case Key::F5:  return "F5";  case Key::F6:  return "F6";
    case Key::F7:  return "F7";  case Key::F8:  return "F8";
    case Key::F9:  return "F9";  case Key::F10: return "F10";
    case Key::F11: return "F11"; case Key::F12: return "F12";
    case Key::Unknown: return "None";
    default: {
        const char* n = wsi::keyName(key);
        return n ? n : "?";
    }
    }
}

// ── Settings categories ────────────────────────────────────────────────────
enum class SettingsCategory { General, Input, Physics, Audio, Rendering, Scripting };

// ── Project Settings window ────────────────────────────────────────────────
// Call drawProjectSettings() each frame when open.
// Pass bool* open — set to false when user closes it.
struct ProjectSettingsState {
    SettingsCategory  category   = SettingsCategory::Input;

    // Key capture state
    bool      capturing     = false;
    StringID  captureAction {};
    StringID  captureAxis   {};
    bool      captureIsAxis = false;
    bool      captureIsPos  = false; // for axes: positive or negative slot

    // Add action/axis UI state
    char newActionName[64] = {};
    char newAxisName[64]   = {};
};

struct CategoryInfo { const char* label; SettingsCategory cat; };
inline const std::vector<CategoryInfo>& settingsCategories() {
    static const std::vector<CategoryInfo> c = {
        {"General",   SettingsCategory::General},
        {"Input",     SettingsCategory::Input},
        {"Physics",   SettingsCategory::Physics},
        {"Audio",     SettingsCategory::Audio},
        {"Rendering", SettingsCategory::Rendering},
        {"Scripting", SettingsCategory::Scripting},
    };
    return c;
}

inline void beginCaptureActionKey(ProjectSettingsState& s, StringID action) {
    s.capturing = true; s.captureIsAxis = false; s.captureAction = action;
}
inline void beginCaptureAxisKey(ProjectSettingsState& s, StringID axis, bool positive) {
    s.capturing = true; s.captureIsAxis = true; s.captureIsPos = positive; s.captureAxis = axis;
}

// The key the user pressed while a capture was waiting. Escape cancels;
// anything else is bound. False when nothing was waiting.
inline bool acceptCapturedKey(ProjectSettingsState& s, Key k) {
    if (!s.capturing || k == Key::Unknown) return false;
    if (k != Key::Escape) {
        auto& map = InputMap::get();
        if (!s.captureIsAxis)      map.addActionKey(s.captureAction, k);
        else if (s.captureIsPos)   map.setAxisPositive(s.captureAxis, k);
        else                       map.setAxisNegative(s.captureAxis, k);
    }
    s.capturing = false;
    return true;
}
