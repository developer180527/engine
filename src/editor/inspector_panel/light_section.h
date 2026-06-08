#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/light.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawLightSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Light>()) return;

    sectionHeader("Light");
    Light& lt = e.get_mut<Light>();

    const char* lightTypes[] = {"Directional", "Point", "Spot"};
    int lti = (int)lt.type;
    ImGui::Text("Type"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    { auto bk = UndoStack::snapshotComponent(e, "light");
      if (ImGui::Combo("##ltype", &lti, lightTypes, 3)) {
          lt.type = (LightType)lti;
          ctx.editor.undoStack.pushPropertyEdit(e, "light", bk,
              UndoStack::snapshotComponent(e, "light"), "Change Light type");
          ctx.editor.sceneDirty = true;
      }
    }

    ImGui::Text("Use Temp"); ImGui::SameLine(90.0f);
    { auto bk = UndoStack::snapshotComponent(e, "light");
      if (ImGui::Checkbox("##lusetemp", &lt.useTemperature)) {
          ctx.editor.undoStack.pushPropertyEdit(e, "light", bk,
              UndoStack::snapshotComponent(e, "light"), "Toggle temperature mode");
          ctx.editor.sceneDirty = true;
      }
    }
    ImGui::SameLine(); ImGui::TextDisabled(lt.useTemperature ? "Kelvin" : "RGB");

    if (lt.useTemperature) {
        ImGui::Text("Kelvin"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##lkelvin", &lt.temperatureK, 1500.0f, 15000.0f, "%.0f K");
        propEdit(ctx, e, "light", "Edit Light");
        ImGui::Text("Tint"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3("##ltint", &lt.color.x, ImGuiColorEditFlags_Float);
        propEdit(ctx, e, "light", "Edit Light");
        bx::Vec3 kc = kelvinToRGB(lt.temperatureK);
        ImVec4 result = { kc.x*lt.color.x, kc.y*lt.color.y, kc.z*lt.color.z, 1.0f };
        ImGui::Text("Result"); ImGui::SameLine(90.0f);
        ImGui::ColorButton("##lresult", result, ImGuiColorEditFlags_NoTooltip,
                           ImVec2(ImGui::GetContentRegionAvail().x, 0));
    } else {
        ImGui::Text("Color"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit3("##lcolor", &lt.color.x, ImGuiColorEditFlags_Float);
        propEdit(ctx, e, "light", "Edit Light");
    }

    float intensMax = (lt.type == LightType::Directional) ? 10.0f : 100.0f;
    ImGui::Text("Intensity"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##lintensity", &lt.intensity, 0.0f, intensMax, "%.2f");
    propEdit(ctx, e, "light", "Edit Light");

    if (lt.type != LightType::Directional) {
        ImGui::Text("Range"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##lrange", &lt.range, 0.1f, 0.1f, 1000.0f, "%.1f");
        propEdit(ctx, e, "light", "Edit Light");
    }

    if (lt.type == LightType::Spot) {
        ImGui::Text("Inner"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##lspotin", &lt.spotInner, 0.0f, lt.spotOuter, "%.1f deg");
        propEdit(ctx, e, "light", "Edit Light");
        ImGui::Text("Outer"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##lspotout", &lt.spotOuter, lt.spotInner, 89.0f, "%.1f deg");
        propEdit(ctx, e, "light", "Edit Light");
        ImGui::TextDisabled("(cone approximate until spot shading lands)");
    }

    ImGui::Text("Shadows"); ImGui::SameLine(90.0f);
    { auto bk = UndoStack::snapshotComponent(e, "light");
      if (ImGui::Checkbox("##lshadow", &lt.castShadows)) {
          ctx.editor.undoStack.pushPropertyEdit(e, "light", bk,
              UndoStack::snapshotComponent(e, "light"), "Toggle shadows");
          ctx.editor.sceneDirty = true;
      }
    }
    if (lt.type != LightType::Directional) {
        ImGui::SameLine(); ImGui::TextDisabled("(directional only)");
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
    if (ImGui::Button("Remove Light", {-1, 0})) {
        ctx.editor.undoStack.pushComponentRemove(e, "light", "Remove Light");
        e.remove<Light>(); ctx.editor.sceneDirty = true;
    }
    ImGui::PopStyleColor();
}

} // namespace inspector_detail
