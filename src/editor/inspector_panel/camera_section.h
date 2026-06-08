#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/camera.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawCameraSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Camera>()) return;

    sectionHeader("Camera");
    Camera& cam = e.get_mut<Camera>();

    // Primary
    ImGui::Text("Primary");
    ImGui::SameLine(90.0f);
    { auto bk = UndoStack::snapshotComponent(e, "camera");
      if (ImGui::Checkbox("##primary", &cam.isPrimary)) {
          ctx.editor.undoStack.pushPropertyEdit(e, "camera", bk,
              UndoStack::snapshotComponent(e, "camera"), "Toggle primary camera");
          ctx.editor.sceneDirty = true;
      }
    }
    if (cam.isPrimary) {
        ImGui::SameLine();
        ImGui::TextColored({0.3f,0.9f,0.3f,1}, "Game Camera");
    }

    // Projection
    ImGui::Text("Projection");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(-1);
    const char* projNames[] = {"Perspective", "Orthographic"};
    int projIdx = (int)cam.projection;
    { auto bk = UndoStack::snapshotComponent(e, "camera");
      if (ImGui::Combo("##proj", &projIdx, projNames, 2)) {
          cam.projection = (ProjectionType)projIdx;
          ctx.editor.undoStack.pushPropertyEdit(e, "camera", bk,
              UndoStack::snapshotComponent(e, "camera"), "Change projection");
          ctx.editor.sceneDirty = true;
      }
    }

    if (cam.projection == ProjectionType::Perspective) {
        ImGui::Text("FOV");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##fov", &cam.fov, 10.0f, 170.0f, "%.1f deg");
        propEdit(ctx, e, "camera", "Edit FOV");
    } else {
        ImGui::Text("Size");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##orthoSize", &cam.orthoSize, 0.1f, 0.1f, 1000.0f);
        propEdit(ctx, e, "camera", "Edit ortho size");
    }

    ImGui::Text("Near");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##near", &cam.nearPlane, 0.001f, 0.001f, 100.0f, "%.3f");
    propEdit(ctx, e, "camera", "Edit near plane");

    ImGui::Text("Far");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##far", &cam.farPlane, 1.0f, 1.0f, 100000.0f);
    propEdit(ctx, e, "camera", "Edit far plane");

    ImGui::Spacing();
    ImGui::Text("Clear");
    ImGui::SameLine(90.0f);
    ImGui::SetNextItemWidth(-1);
    ImGui::ColorEdit4("##clear", cam.clearColor,
        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
    propEdit(ctx, e, "camera", "Edit clear color");
}

} // namespace inspector_detail
