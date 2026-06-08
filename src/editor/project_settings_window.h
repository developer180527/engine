#pragma once
#include <imgui.h>
#include "editor/editor_icons.h"
#include <string>
#include <cstring>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "engine/input_map.h"
#include "engine/input_system.h"
#include "editor/editor_state.h"

// ── Key display name ───────────────────────────────────────────────────────
inline const char* keyDisplayName(int glfwKey) {
    switch (glfwKey) {
    case GLFW_KEY_SPACE:         return "Space";
    case GLFW_KEY_ENTER:         return "Enter";
    case GLFW_KEY_ESCAPE:        return "Escape";
    case GLFW_KEY_TAB:           return "Tab";
    case GLFW_KEY_BACKSPACE:     return "Backspace";
    case GLFW_KEY_DELETE:        return "Delete";
    case GLFW_KEY_RIGHT:         return "Right";
    case GLFW_KEY_LEFT:          return "Left";
    case GLFW_KEY_UP:            return "Up";
    case GLFW_KEY_DOWN:          return "Down";
    case GLFW_KEY_LEFT_SHIFT:    return "L.Shift";
    case GLFW_KEY_RIGHT_SHIFT:   return "R.Shift";
    case GLFW_KEY_LEFT_CONTROL:  return "L.Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "R.Ctrl";
    case GLFW_KEY_LEFT_ALT:      return "L.Alt";
    case GLFW_KEY_RIGHT_ALT:     return "R.Alt";
    case GLFW_KEY_LEFT_SUPER:    return "L.Cmd";
    case GLFW_KEY_RIGHT_SUPER:   return "R.Cmd";
    case GLFW_KEY_F1:  return "F1";  case GLFW_KEY_F2:  return "F2";
    case GLFW_KEY_F3:  return "F3";  case GLFW_KEY_F4:  return "F4";
    case GLFW_KEY_F5:  return "F5";  case GLFW_KEY_F6:  return "F6";
    case GLFW_KEY_F7:  return "F7";  case GLFW_KEY_F8:  return "F8";
    case GLFW_KEY_F9:  return "F9";  case GLFW_KEY_F10: return "F10";
    case GLFW_KEY_F11: return "F11"; case GLFW_KEY_F12: return "F12";
    case GLFW_KEY_UNKNOWN: return "None";
    default: {
        const char* n = glfwGetKeyName(glfwKey, 0);
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

inline void drawCategoryInput(ProjectSettingsState& s, SimState simState) {
    auto& map = InputMap::get();
    auto& sys = InputSystem::get();

    // ── Key capture overlay ───────────────────────────────────────────────
    if (s.capturing) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f,0.15f,0.15f,1.0f));
        ImGui::BeginChild("##capture", ImVec2(0, 48), true);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - 160) * 0.5f);
        ImGui::TextColored({1.0f,0.8f,0.2f,1.0f}, "Press any key to bind...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) s.capturing = false;
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Check for key
        int k = sys.anyKeyPressedRaw();
        if (k >= 0 && k != GLFW_KEY_ESCAPE) {
            if (!s.captureIsAxis) {
                map.addActionKey(s.captureAction, (Key)k);
            } else if (s.captureIsPos) {
                map.setAxisPositive(s.captureAxis, (Key)k);
            } else {
                map.setAxisNegative(s.captureAxis, (Key)k);
            }
            s.capturing = false;
        } else if (k == GLFW_KEY_ESCAPE) {
            s.capturing = false;
        }
        ImGui::Spacing();
    }

    // ── Actions ───────────────────────────────────────────────────────────
    ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Actions");
    ImGui::Separator();

    constexpr ImGuiTableFlags kTbl =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##actions", 3, kTbl)) {
        ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();

        StringID toRemoveAction{};
        for (auto& action : map.actions()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action.name.c_str());

            ImGui::TableSetColumnIndex(1);
            for (int ki = 0; ki < (int)action.keys.size(); ++ki) {
                ImGui::PushID(ki);
                // Key badge
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.25f,0.35f,0.55f,1.0f));
                const char* kn = keyDisplayName((int)action.keys[ki]);
                if (ImGui::SmallButton(kn))
                    map.removeActionKey(action.id, ki);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to remove");
                ImGui::SameLine(0, 4);
                ImGui::PopID();
            }
            // + Add binding
            ImGui::PushID((int)action.id.id);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.4f,0.2f,1.0f));
            if (ImGui::SmallButton("+")) {
                s.capturing      = true;
                s.captureIsAxis  = false;
                s.captureAction  = action.id;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((int)action.id.id + 10000);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,0.8f));
            if (ImGui::SmallButton("x")) toRemoveAction = action.id;
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (toRemoveAction.id) map.removeAction(toRemoveAction);
        ImGui::EndTable();
    }

    // Add action row
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##newaction", s.newActionName, sizeof(s.newActionName));
    ImGui::SameLine();
    if (ImGui::Button("+ Add Action") && s.newActionName[0]) {
        map.addAction(s.newActionName);
        s.newActionName[0] = '\0';
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Axes ──────────────────────────────────────────────────────────────
    ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Axes");
    ImGui::Separator();

    if (ImGui::BeginTable("##axes", 4, kTbl)) {
        ImGui::TableSetupColumn("Axis",     ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableSetupColumn("+ (pos)",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("- (neg)",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   24.0f);
        ImGui::TableHeadersRow();

        StringID toRemoveAxis{};
        for (auto& axis : map.axes()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(axis.name.c_str());

            // Positive key
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID((int)axis.id.id + 20000);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.45f,0.15f,1.0f));
            if (ImGui::Button(keyDisplayName((int)axis.positive))) {
                s.capturing      = true;
                s.captureIsAxis  = true;
                s.captureIsPos   = true;
                s.captureAxis    = axis.id;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();

            // Negative key
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((int)axis.id.id + 30000);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f,0.15f,0.15f,1.0f));
            if (ImGui::Button(keyDisplayName((int)axis.negative))) {
                s.capturing      = true;
                s.captureIsAxis  = true;
                s.captureIsPos   = false;
                s.captureAxis    = axis.id;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();

            ImGui::TableSetColumnIndex(3);
            ImGui::PushID((int)axis.id.id + 40000);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,0.8f));
            if (ImGui::SmallButton("x")) toRemoveAxis = axis.id;
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (toRemoveAxis.id) map.removeAxis(toRemoveAxis);
        ImGui::EndTable();
    }

    // Add axis row
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##newaxis", s.newAxisName, sizeof(s.newAxisName));
    ImGui::SameLine();
    if (ImGui::Button("+ Add Axis") && s.newAxisName[0]) {
        map.addAxis(s.newAxisName, Key::Unknown, Key::Unknown);
        s.newAxisName[0] = '\0';
    }

    // ── Live test ─────────────────────────────────────────────────────────
    ImGui::Spacing(); ImGui::Spacing();
    ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Live Test");
    ImGui::Separator();
    if (simState != SimState::Playing) {
        ImGui::TextDisabled("Enter Play mode to test axis values live.");
    } else {
        for (const auto& axis : map.axes()) {
            float v = map.getAxis(axis.id);
            char label[64]; snprintf(label, sizeof(label), "%-16s  %.2f", axis.name.c_str(), v);
            float display = (v + 1.0f) * 0.5f; // map -1..1 to 0..1 for progress bar
            ImGui::ProgressBar(display, ImVec2(-1, 0), label);
        }
        for (const auto& action : map.actions()) {
            bool down = map.isActionDown(action.id);
            ImGui::TextColored(down ? ImVec4{0.3f,1.0f,0.4f,1.0f}
                                    : ImVec4{0.5f,0.5f,0.5f,1.0f},
                "%-16s  %s", action.name.c_str(), down ? "DOWN" : "up");
        }
    }
}

inline void drawProjectSettings(bool* open, SimState simState,
                                 ProjectSettingsState& s) {
    if (!*open) return;

    ImGui::SetNextWindowSize(ImVec2(860, 580), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f - 430,
               ImGui::GetIO().DisplaySize.y * 0.5f - 290),
        ImGuiCond_FirstUseEver);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

    if (!ImGui::Begin(ICON_FA_GEAR " Project Settings", open, kFlags)) {
        ImGui::End(); return;
    }

    // ── Sidebar ───────────────────────────────────────────────────────────
    ImGui::BeginChild("##sidebar", ImVec2(160, 0), true);

    struct { const char* label; SettingsCategory cat; } categories[] = {
        {"General",   SettingsCategory::General},
        {"Input",     SettingsCategory::Input},
        {"Physics",   SettingsCategory::Physics},
        {"Audio",     SettingsCategory::Audio},
        {"Rendering", SettingsCategory::Rendering},
        {"Scripting", SettingsCategory::Scripting},
    };
    for (auto& c : categories) {
        bool sel = (s.category == c.cat);
        if (ImGui::Selectable(c.label, sel))
            s.category = c.cat;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ── Right panel ───────────────────────────────────────────────────────
    ImGui::BeginChild("##panel", ImVec2(0, -32), false);
    ImGui::Spacing();

    switch (s.category) {
    case SettingsCategory::General:
        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "General");
        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("Project Name, version, default scene — coming soon.");
        break;
    case SettingsCategory::Input:
        drawCategoryInput(s, simState);
        break;
    case SettingsCategory::Physics:
        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Physics");
        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("Gravity, collision layers — available after Jolt integration.");
        break;
    case SettingsCategory::Audio:
        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Audio");
        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("Master volume, channels — coming soon.");
        break;
    case SettingsCategory::Rendering:
        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Rendering");
        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("Shadow quality, MSAA, LOD bias — coming soon.");
        break;
    case SettingsCategory::Scripting:
        ImGui::TextColored({0.6f,0.85f,1.0f,1.0f}, "Scripting");
        ImGui::Separator(); ImGui::Spacing();
        ImGui::TextDisabled("LuaJIT settings, script root path — available after scripting integration.");
        break;
    }

    ImGui::EndChild();

    // ── Bottom bar ────────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults")) {
        auto& map = InputMap::get();
        map.clearAll();
        map.bindAction("Jump",     Key::Space);
        map.bindAction("Interact", Key::F);
        map.bindAction("Sprint",   Key::LeftShift);
        map.bindAxis("MoveForward", Key::W, Key::S);
        map.bindAxis("MoveRight",   Key::D, Key::A);
        map.bindAxis("MoveUp",      Key::E, Key::Q);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Changes apply immediately.");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 48);
    if (ImGui::Button("Close")) *open = false;

    ImGui::End();
}
