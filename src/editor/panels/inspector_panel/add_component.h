#pragma once
// ── Add Component menu ───────────────────────────────────────────────────────
// One "+ Add Component" button that opens a searchable popup of the components
// the selected entity can still take (Unity-style), replacing the per-component
// "+ Add X" buttons that used to live in each section.
//
// The registry below is a hand-rolled component table — `canAdd` encodes rules
// like RigidBody xor CharacterController. (This is the manual stand-in for the
// reflection system that will eventually drive add/serialize/inspect from one
// component declaration.)
//
// Deliberately NOT listed: Camera (entities with cameras are created from the
// Hierarchy; primary-camera semantics), Spinner (a demo component the scene
// serializer skips — adding it would silently stop the entity from saving).
#include <imgui.h>
#include <flecs.h>
#include <vector>
#include <string>
#include <cctype>

#include "editor/engine_context.h"
#include "components/rigid_body.h"
#include "components/character_controller.h"
#include "components/light.h"
#include "components/animator.h"
#include "components/script_component.h"

namespace inspector_detail {

struct AddableComponent {
    const char* label;                  // shown in the menu
    const char* key;                    // undo key (matches the serializer key)
    bool (*canAdd)(flecs::entity);      // false once present / mutually excluded
    void (*add)(flecs::entity);         // attach with sane defaults
};

inline const std::vector<AddableComponent>& addableComponents() {
    static const std::vector<AddableComponent> list = {
        { "Light", "light",
          [](flecs::entity e){ return !e.has<Light>(); },
          [](flecs::entity e){ e.set<Light>({}); } },
        { "Rigid Body", "rigidBody",
          [](flecs::entity e){ return !e.has<RigidBody>() && !e.has<CharacterController>(); },
          [](flecs::entity e){ e.set<RigidBody>({}); } },
        { "Character Controller", "characterController",
          [](flecs::entity e){ return !e.has<CharacterController>() && !e.has<RigidBody>(); },
          [](flecs::entity e){ e.set<CharacterController>({}); } },
        { "Animator", "animator",
          [](flecs::entity e){ return !e.has<Animator>(); },
          [](flecs::entity e){ e.set<Animator>({}); } },
        { "Script", "script",
          [](flecs::entity e){ return !e.has<ScriptComponent>(); },
          [](flecs::entity e){ e.set<ScriptComponent>({}); } },
    };
    return list;
}

inline bool matchesFilter(const char* label, const char* filter) {
    if (!filter[0]) return true;
    std::string h(label), n(filter);
    auto lower = [](std::string& s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); };
    lower(h); lower(n);
    return h.find(n) != std::string::npos;
}

inline void drawAddComponentButton(EngineContext& ctx, flecs::entity e) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("+ Add Component", {-1, 0}))
        ImGui::OpenPopup("##addComponent");

    if (ImGui::BeginPopup("##addComponent")) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) {
            filter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##search", "Search...", filter, sizeof(filter));
        ImGui::Separator();

        bool any = false;
        for (const auto& c : addableComponents()) {
            if (!c.canAdd(e))                    continue;  // already present / excluded
            if (!matchesFilter(c.label, filter)) continue;
            any = true;
            if (ImGui::Selectable(c.label)) {
                c.add(e);
                ctx.editor.undoStack.pushComponentAdd(e, c.key, std::string("Add ") + c.label);
                ctx.editor.sceneDirty = true;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!any)
            ImGui::TextDisabled(filter[0] ? "No matching component" : "All components added");

        ImGui::EndPopup();
    }
}

} // namespace inspector_detail
