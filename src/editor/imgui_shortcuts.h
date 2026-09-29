#pragma once
// ── imgui_shortcuts — ImGui's key state -> editor commands ──────────────────
// The ImGui editor's adapter onto editor/core/shortcuts.h. ImGui's backends
// already own the keyboard here, so this reads ImGui's key state rather than
// the platform's UI input stream (which the libgui front end uses): same
// ShortcutMap, different source of key presses.
#include "editor/core/shortcuts.h"

#include <imgui.h>

#include <string_view>

namespace imgui_shortcuts {

// ImGuiKey -> engine Key, for every key a shortcut can name. Physical keys,
// as ImGui's own named keys are.
inline Key toKey(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z)   return (Key)((int)Key::A + (k - ImGuiKey_A));
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9)   return (Key)((int)Key::Num0 + (k - ImGuiKey_0));
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return (Key)((int)Key::F1 + (k - ImGuiKey_F1));
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return (Key)((int)Key::Kp0 + (k - ImGuiKey_Keypad0));
    switch (k) {
    case ImGuiKey_Space:        return Key::Space;
    case ImGuiKey_Escape:       return Key::Escape;
    case ImGuiKey_Enter:        return Key::Enter;
    case ImGuiKey_Tab:          return Key::Tab;
    case ImGuiKey_Backspace:    return Key::Backspace;
    case ImGuiKey_Delete:       return Key::Delete;
    case ImGuiKey_Insert:       return Key::Insert;
    case ImGuiKey_LeftArrow:    return Key::Left;
    case ImGuiKey_RightArrow:   return Key::Right;
    case ImGuiKey_UpArrow:      return Key::Up;
    case ImGuiKey_DownArrow:    return Key::Down;
    case ImGuiKey_PageUp:       return Key::PageUp;
    case ImGuiKey_PageDown:     return Key::PageDown;
    case ImGuiKey_Home:         return Key::Home;
    case ImGuiKey_End:          return Key::End;
    case ImGuiKey_Apostrophe:   return Key::Apostrophe;
    case ImGuiKey_Comma:        return Key::Comma;
    case ImGuiKey_Minus:        return Key::Minus;
    case ImGuiKey_Period:       return Key::Period;
    case ImGuiKey_Slash:        return Key::Slash;
    case ImGuiKey_Semicolon:    return Key::Semicolon;
    case ImGuiKey_Equal:        return Key::Equal;
    case ImGuiKey_LeftBracket:  return Key::LeftBracket;
    case ImGuiKey_Backslash:    return Key::Backslash;
    case ImGuiKey_RightBracket: return Key::RightBracket;
    case ImGuiKey_GraveAccent:  return Key::GraveAccent;
    case ImGuiKey_KeypadDecimal:  return Key::KpDecimal;
    case ImGuiKey_KeypadDivide:   return Key::KpDivide;
    case ImGuiKey_KeypadMultiply: return Key::KpMultiply;
    case ImGuiKey_KeypadSubtract: return Key::KpSubtract;
    case ImGuiKey_KeypadAdd:      return Key::KpAdd;
    case ImGuiKey_KeypadEnter:    return Key::KpEnter;
    case ImGuiKey_KeypadEqual:    return Key::KpEqual;
    default:                    return Key::Unknown;
    }
}

// Physical modifiers, read from the modifier KEYS rather than io.KeyCtrl /
// io.KeySuper: some ImGui versions swap those two on macOS so that "Ctrl"
// means Cmd, and the shortcut layer does that mapping itself — reading the
// swapped flags would apply it twice.
inline uiin::Modifiers modifiers() {
    uiin::Modifiers m;
    m.shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    m.ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    m.alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);
    m.super = ImGui::IsKeyDown(ImGuiKey_LeftSuper) || ImGui::IsKeyDown(ImGuiKey_RightSuper);
    return m;
}

// Every command triggered this frame, in press order, passed to `fire`.
template <class Fire>
void dispatch(const shortcuts::ShortcutMap& map, Fire&& fire) {
    const uiin::Modifiers mods = modifiers();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey ik = (ImGuiKey)k;
        const Key key = toKey(ik);
        if (key == Key::Unknown) continue;
        if (ImGui::IsKeyPressed(ik, /*repeat*/ false)) {
            if (std::string_view id = map.match(key, mods, false); !id.empty()) fire(id);
        } else if (ImGui::IsKeyPressed(ik, /*repeat*/ true)) {
            if (std::string_view id = map.match(key, mods, true); !id.empty()) fire(id);
        }
    }
}

}  // namespace imgui_shortcuts
