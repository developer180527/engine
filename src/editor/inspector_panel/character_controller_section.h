#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/character_controller.h"
#include "components/rigid_body.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawCharacterControllerSection(EngineContext& ctx, flecs::entity e) {
    if (e.has<CharacterController>()) {
        sectionHeader("Character Controller");
        CharacterController& cc = e.get_mut<CharacterController>();
        ImGui::Text("Radius");    ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ccr", &cc.radius, 0.01f, 0.05f, 5.f);
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::Text("Height");    ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##cch", &cc.height, 0.01f, 0.1f, 10.f);
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::Text("Max Slope"); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##ccslope", &cc.maxSlopeDeg, 0.f, 80.f, "%.0f deg");
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::Text("Step Up");   ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ccstep", &cc.stepHeight, 0.01f, 0.f, 2.f);
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::Text("Mass");      ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ccmass", &cc.mass, 1.f, 1.f, 1000.f, "%.0f kg");
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::Text("Gravity x"); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##ccgrav", &cc.gravityScale, 0.05f, 0.f, 5.f);
        propEdit(ctx, e, "characterController", "Edit Character");
        ImGui::TextDisabled("Grounded: %s", cc.grounded ? "yes" : "no");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove Character Controller", {-1, 0})) {
            ctx.editor.undoStack.pushComponentRemove(e, "characterController", "Remove Character Controller");
            e.remove<CharacterController>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    } else if (!e.has<RigidBody>()) {
        ImGui::Spacing();
        if (ImGui::Button("+ Add Character Controller", {-1, 0})) {
            e.set<CharacterController>({});
            ctx.editor.undoStack.pushComponentAdd(e, "characterController", "Add Character Controller");
            ctx.editor.sceneDirty = true;
        }
    }
}

} // namespace inspector_detail
