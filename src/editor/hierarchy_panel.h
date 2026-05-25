#pragma once
#include <flecs.h>
#include <imgui.h>
#include "engine_context.h"
#include "components/name.h"
#include "components/mesh_renderer.h"

inline void drawHierarchyPanel(EngineContext& ctx) {
    ImGui::Begin("Hierarchy");

    flecs::entity toDelete{};

    ctx.ecs.query_builder<const Name>()
        .build()
        .each([&](flecs::entity e, const Name& n) {
            const bool isSelected = (ctx.editor.selected == e);
            ImGui::PushID(static_cast<int>(e.id()));

            if (ImGui::Selectable(n.value.c_str(), isSelected))
                ctx.editor.selected = e;

            // Right-click context menu per entity
            if (ImGui::BeginPopupContextItem("##ctx")) {
                ImGui::TextDisabled("%s", n.value.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del"))
                    toDelete = e;
                if (ImGui::MenuItem("Select"))
                    ctx.editor.selected = e;
                ImGui::EndPopup();
            }

            ImGui::PopID();
        });

    // Delete key while hierarchy window is focused
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        ctx.editor.selected.is_alive())
        toDelete = ctx.editor.selected;

    // Perform deletion outside the ECS query to avoid iterator invalidation
    if (toDelete.is_alive()) {
        if (ctx.editor.selected == toDelete)
            ctx.editor.selected = {};
        toDelete.destruct();
        ctx.editor.sceneDirty = true;
    }

    // Empty-area right-click (deselect)
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered())
        ctx.editor.selected = {};

    ImGui::End();
}
