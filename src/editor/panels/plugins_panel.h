#pragma once
#include <imgui.h>
#include <filesystem>
#include "editor/editor_icons.h"
#include "editor/editor_plugin.h"
#include "runtime/plugin_registry.h"
#include "runtime/kit_host.h"
#include "project/project_context.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"

// ── Plug-in Manager ──────────────────────────────────────────────────────────
//   Running  — what's attached to the runtime RIGHT NOW (registry.all()).
//   Kits     — the project manifest, each row showing its TRUE load result
//              (from KitHost::status() while playing), not a guess from "is the
//              sim running".
namespace {
inline const ImVec4 kGreen{0.30f, 1.00f, 0.42f, 1.0f};
inline const ImVec4 kRed  {1.00f, 0.42f, 0.42f, 1.0f};

inline bool isBuiltinPlugin(IEnginePlugin* p) {
    return dynamic_cast<JoltPlugin*>(p)
        || dynamic_cast<LuaScriptPlugin*>(p)
        || dynamic_cast<AudioPlugin*>(p);
}
// Mirror KitHost::resolve so the panel shows the same path the loader uses.
inline std::filesystem::path resolveKitModule(const ProjectContext& project,
                                              const std::string& module) {
    std::filesystem::path p(module);
    return p.is_absolute() ? p : (project.projectRoot / p);
}
} // namespace

inline void drawPluginsPanel(bool* open, PluginRegistry& plugins,
                             ProjectContext& project, const KitHost& kits,
                             bool simulating) {
    if (open && !*open) return;
    // No close button on the title bar — visibility is the Plugins menu's job.
    if (!ImGui::Begin(ICON_FA_SCREWDRIVER_WRENCH " Plug-in Manager")) {
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

            p->onEditorUI();   // draws via the engineUi* facade, or no-ops
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

            // The TRUTH while playing (empty while idle).
            const KitHost::KitStatus* st = nullptr;
            for (const auto& s : kits.status())
                if (s.name == k.name) { st = &s; break; }

            bool en = k.enabled;
            if (ImGui::Checkbox("##enabled", &en)) { k.enabled = en; dirty = true; }
            ImGui::SameLine();
            ImGui::TextUnformatted(k.name.empty() ? k.module.c_str() : k.name.c_str());
            ImGui::SameLine();

            const std::filesystem::path full = resolveKitModule(project, k.module);
            const char* errLine = nullptr;
            if (!k.enabled) {
                ImGui::TextColored(kRed, ICON_FA_CIRCLE_XMARK " disabled");
            } else if (st) {                              // playing: real result
                using S = KitHost::KitStatus::State;
                switch (st->state) {
                    case S::Loaded:
                        ImGui::TextColored(kGreen, ICON_FA_CIRCLE_CHECK " loaded"); break;
                    case S::FileNotFound:
                        ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " missing");
                        errLine = st->message.c_str(); break;
                    case S::LoadFailed:
                        ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " failed");
                        errLine = st->message.c_str(); break;
                }
            } else if (!std::filesystem::exists(full)) {  // idle pre-flight
                ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " path not found");
                errLine = "module file does not exist at this path";
            } else {
                ImGui::TextDisabled(ICON_FA_CIRCLE_PLAY " loads at Play");
            }

            ImGui::TextDisabled("      %s", full.string().c_str());
            if (errLine) ImGui::TextColored(kRed, "      %s", errLine);
            ImGui::PopID();
        }
        if (dirty) project.save();   // persist enable/disable to project.json
        if (simulating)
            ImGui::TextDisabled("Enable/disable applies on the next Play.");
    }

    ImGui::End();
}

// ── Kit-load failure modal ───────────────────────────────────────────────────
// Call every frame; set *show=true (e.g. on Play) to raise it when kits failed.
inline void drawKitErrorModal(bool* show, const KitHost& kits) {
    if (show && *show) { ImGui::OpenPopup("Kit load failed"); *show = false; }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Kit load failed", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION
                           " Some kits did not load — gameplay may be missing.");
        ImGui::Spacing();
        for (const auto& s : kits.status()) {
            if (s.state == KitHost::KitStatus::State::Loaded) continue;
            ImGui::BulletText("%s", s.name.c_str());
            ImGui::Indent();
            ImGui::TextDisabled("%s", s.message.c_str());
            ImGui::TextWrapped("%s", s.resolvedPath.string().c_str());
            ImGui::Unindent();
            ImGui::Spacing();
        }
        ImGui::Separator();
        if (ImGui::Button("OK", {120, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
