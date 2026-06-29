#pragma once

#include <cstdio>
#include <imgui.h>
#include <flecs.h>
#include <filesystem>

#include "editor/engine_context.h"
#include "components/script_component.h"
#include "editor/panels/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawScriptSection(EngineContext& ctx, flecs::entity e) {
    namespace fsi = std::filesystem;
    static std::vector<std::pair<std::string,std::string>> s_scriptList;

    // Drop target: accept an ASSET_PATH payload if it's a .lua and store it as
    // a path relative to the project root (so it always resolves at Play).
    auto acceptScriptDrop = [&](ScriptComponent* scPtr) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            fsi::path ap((const char*)pl->Data);
            if (ap.extension() == ".lua") {
                std::error_code ec;
                auto rel = fsi::relative(ap, ctx.project.projectRoot, ec);
                if (!ec) {
                    if (!scPtr) { e.set<ScriptComponent>({}); scPtr = &e.get_mut<ScriptComponent>(); }
                    scPtr->scriptPath = rel.generic_string();
                    ctx.editor.sceneDirty = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    };

    if (e.has<ScriptComponent>()) {
        sectionHeader("Script");
        ScriptComponent& sc = e.get_mut<ScriptComponent>();

        ImGui::TextDisabled("Path (relative to project root)");
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", sc.scriptPath.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##scriptPath", buf, sizeof(buf)))
            sc.scriptPath = buf;
        propEdit(ctx, e, "script", "Edit script path");
        acceptScriptDrop(&sc);

        bool valid = false;
        if (!sc.scriptPath.empty()) {
            std::error_code ec;
            valid = fsi::is_regular_file(ctx.project.projectRoot / sc.scriptPath, ec);
        }
        if (sc.scriptPath.empty())
            ImGui::TextDisabled("Drag a .lua here, or Pick");
        else if (valid)
            ImGui::TextColored({0.4f,0.9f,0.4f,1}, "* Found");
        else
            ImGui::TextColored({0.95f,0.45f,0.45f,1}, "* Not found at this path");

        if (ImGui::Button("Pick...")) {
            s_scriptList = scanLuaScripts(ctx.project.projectRoot);
            ImGui::OpenPopup("##pickscript");
        }
        if (ImGui::BeginPopup("##pickscript")) {
            ImGui::TextDisabled("Lua scripts in project");
            ImGui::Separator();
            if (s_scriptList.empty()) ImGui::TextDisabled("(none found)");
            for (auto& sp : s_scriptList)
                if (ImGui::Selectable(sp.first.c_str())) {
                    sc.scriptPath = sp.second; ctx.editor.sceneDirty = true;
                    ImGui::CloseCurrentPopup();
                }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove Script", {-1, 0})) {
            ctx.editor.undoStack.pushComponentRemove(e, "script", "Remove Script");
            e.remove<ScriptComponent>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    }
    // Adding is handled by the unified "+ Add Component" menu (add_component.h).
}

} // namespace inspector_detail
