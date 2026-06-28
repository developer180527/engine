#pragma once
#include <imgui.h>
#include <filesystem>
#include "editor/editor_icons.h"
#include "editor/editor_plugin.h"
#include "runtime/plugin_registry.h"
#include "project/project_context.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"

// ── Plug-in Manager ──────────────────────────────────────────────────────────
// Two views of the same picture:
//   Running  — what's attached to the runtime RIGHT NOW (registry.all()). The
//              stock plugins (physics/scripting/audio) plus, while playing, the
//              project's kits. Stock plugins render their stats; third-party
//              plugins that implement IEditorPlugin get onEditorUI() called.
//   Kits     — the project manifest (project.json). Kits load LAZILY at Play,
//              so this is where you enable/disable them while editing.
namespace {
inline const ImVec4 kGreen{0.30f, 1.00f, 0.42f, 1.0f};
inline const ImVec4 kRed  {1.00f, 0.42f, 0.42f, 1.0f};

inline bool isBuiltinPlugin(IEnginePlugin* p) {
    return dynamic_cast<JoltPlugin*>(p)
        || dynamic_cast<LuaScriptPlugin*>(p)
        || dynamic_cast<AudioPlugin*>(p);
}
} // namespace

inline void drawPluginsPanel(bool* open, PluginRegistry& plugins,
                             ProjectContext& project, bool simulating) {
    if (open && !*open) return;
    if (!ImGui::Begin(ICON_FA_SCREWDRIVER_WRENCH " Plug-in Manager", open)) {
        ImGui::End();
        return;
    }

    // ── Running ──────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Running");
    ImGui::TextDisabled("%zu attached%s", plugins.all().size(),
                        simulating ? "  ·  simulating" : "");
    for (auto& p : plugins.all()) {
        ImGui::PushID(p.get());
        if (ImGui::CollapsingHeader(p->name(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled("%s  ·  v%s",
                isBuiltinPlugin(p.get()) ? "Built-in" : "Kit / module",
                p->version());
            if (simulating)
                ImGui::TextColored(kGreen, ICON_FA_CIRCLE_PLAY " active");
            else
                ImGui::TextDisabled("attached — idle (press Play to simulate)");

            if (auto* jolt = dynamic_cast<JoltPlugin*>(p.get())) {
                if (jolt->simulationActive())
                    ImGui::Text("Bodies:   %d", jolt->bodyCount());
                ImGui::Text("Fixed dt: %.0f Hz", 1.0f / JoltPlugin::fixedTimestep());
                ImGui::TextDisabled("Gravity: (0, -9.81, 0)");
            } else if (auto* lua = dynamic_cast<LuaScriptPlugin*>(p.get())) {
                ImGui::Text("VM:        %s", lua->vmRunning() ? "running" : "off");
                ImGui::Text("Instances: %d", lua->instanceCount());
                ImGui::TextDisabled("Scripts reload on each Play");
            }

            if (auto* ui = dynamic_cast<IEditorPlugin*>(p.get()))
                ui->onEditorUI();
        }
        ImGui::PopID();
    }
    if (plugins.all().empty())
        ImGui::TextDisabled("No plugins registered");

    // ── Kits (project manifest) ──────────────────────────────────────────────
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::SeparatorText("Kits  ·  project manifest");
    ImGui::TextDisabled("Reusable C++ systems this project plugs in. They load at Play.");

    if (project.kits.empty()) {
        ImGui::TextDisabled("No kits declared in project.json");
    } else {
        bool dirty = false;
        for (auto& k : project.kits) {
            ImGui::PushID(&k);
            bool en = k.enabled;
            if (ImGui::Checkbox("##enabled", &en)) { k.enabled = en; dirty = true; }
            ImGui::SameLine();
            ImGui::TextUnformatted(k.name.empty() ? k.module.c_str() : k.name.c_str());

            ImGui::SameLine();
            if (!k.enabled)
                ImGui::TextColored(kRed, ICON_FA_CIRCLE_XMARK " disabled");
            else if (simulating)
                ImGui::TextColored(kGreen, ICON_FA_CIRCLE_CHECK " loaded");
            else
                ImGui::TextDisabled(ICON_FA_CIRCLE_PLAY " loads at Play");

            // Module path + a clear flag if the .so isn't where the manifest says.
            std::filesystem::path mp(k.module);
            std::filesystem::path full =
                mp.is_absolute() ? mp : project.projectRoot / mp;
            ImGui::TextDisabled("      %s", k.module.c_str());
            if (!std::filesystem::exists(full)) {
                ImGui::SameLine();
                ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " missing");
            }
            ImGui::PopID();
        }
        if (dirty) project.save();   // persist enable/disable to project.json
        if (simulating)
            ImGui::TextDisabled("Enable/disable applies on the next Play.");
    }

    ImGui::End();
}
