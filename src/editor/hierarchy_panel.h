#pragma once

#include <flecs.h>
#include <imgui.h>

#include "editor_state.h"
#include "components/name.h"

// Draws an ImGui window listing every entity with a Name component.
// Clicking a row sets `editor.selected` to that entity.
//
// Lives as a free function (not a class method) because it's stateless —
// it just draws what's in the world right now. The editor's persistent
// state lives in EditorState; this function reads selection from there
// and writes selection back to it.
//
// Iteration uses a flecs query each frame. For the entity counts in our
// engine (low hundreds at most) this is trivial; flecs caches the matched
// archetypes between calls, so the cost is just iterating a small list.
inline void drawHierarchyPanel(flecs::world& ecs, EditorState& editor) {
    ImGui::Begin("Hierarchy");

    ecs.query_builder<const Name>()
        .build()
        .each([&](flecs::entity e, const Name& n) {
            const bool isSelected = (editor.selected == e);

            // Use entity ID as a stable ImGui label (entities with same name
            // would otherwise collide in ImGui's internal ID stack).
            ImGui::PushID(static_cast<int>(e.id()));

            if (ImGui::Selectable(n.value.c_str(), isSelected)) {
                editor.selected = e;
            }

            ImGui::PopID();
        });

    ImGui::End();
}
