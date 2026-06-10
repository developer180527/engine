#pragma once
#include <imgui.h>
#include "editor/editor_icons.h"
#include "editor/editor_plugin.h"
#include "runtime/plugin_registry.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"

// ── Plugins panel ──────────────────────────────────────────────────────────
// Lists every plugin registered with the runtime. Stock plugins expose
// UI-free stats getters that this panel renders; third-party plugins that
// implement IEditorPlugin get their onEditorUI() called here.
inline void drawPluginsPanel(PluginRegistry& plugins) {
    ImGui::Begin(ICON_FA_GEAR " Plugins");

    for (auto& p : plugins.all()) {
        ImGui::PushID(p.get());
        if (ImGui::CollapsingHeader(p->name(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("Version: %s", p->version());

            if (auto* jolt = dynamic_cast<JoltPlugin*>(p.get())) {
                if (jolt->simulationActive()) {
                    ImGui::TextColored({0.3f, 1.0f, 0.4f, 1.0f}, "ACTIVE");
                    ImGui::Text("Bodies:   %d", jolt->bodyCount());
                } else {
                    ImGui::TextDisabled("standby — enter play mode to simulate");
                }
                ImGui::Text("Fixed dt: %.0f Hz", 1.0f / JoltPlugin::fixedTimestep());
                ImGui::TextDisabled("Gravity: (0, -9.81, 0)");
            } else if (auto* lua = dynamic_cast<LuaScriptPlugin*>(p.get())) {
                ImGui::Text("VM:        %s", lua->vmRunning() ? "running" : "off");
                ImGui::Text("Instances: %d", lua->instanceCount());
                ImGui::TextDisabled("Lifecycle: onStart / onUpdate / onDestroy");
                ImGui::TextDisabled("Scripts reload on each Play");
            }

            // Third-party plugins draw their own UI via IEditorPlugin
            if (auto* ui = dynamic_cast<IEditorPlugin*>(p.get()))
                ui->onEditorUI();
        }
        ImGui::PopID();
    }

    if (plugins.all().empty())
        ImGui::TextDisabled("No plugins registered");

    ImGui::End();
}
