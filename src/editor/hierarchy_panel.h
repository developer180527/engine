#pragma once

#include <flecs.h>
#include <imgui.h>

#include "engine_context.h"
#include "components/name.h"

inline void drawHierarchyPanel(EngineContext& ctx) {
    ImGui::Begin("Hierarchy");

    ctx.ecs.query_builder<const Name>()
        .build()
        .each([&](flecs::entity e, const Name& n) {
            const bool isSelected = (ctx.editor.selected == e);
            ImGui::PushID(static_cast<int>(e.id()));
            if (ImGui::Selectable(n.value.c_str(), isSelected))
                ctx.editor.selected = e;
            ImGui::PopID();
        });

    ImGui::End();
}
