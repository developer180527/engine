#pragma once
#include <imgui.h>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include "editor/editor_icons.h"
#include "runtime/plugin_registry.h"
#include "runtime/kit_host.h"
#include "project/project_context.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"
#include "editor/panels/plugins/model.h"

// ── Plug-in Manager + per-plugin windows ─────────────────────────────────────
// The manager is a directory: a row per running plugin (with an "Open" button)
// and the project's kit manifest with true load status. Each plugin draws its
// OWN dockable window (stats + its onEditorUI() facade content), so a kit's UI
// is a real, closable, dockable panel — not embedded in the manager.

// name -> visible; owned by the editor, lives across the session.
struct PluginWindows {
    std::unordered_map<std::string, bool> open;
    std::string focus;   // name to bring to front this frame
};

namespace {
inline const ImVec4 kGreen{0.30f, 1.00f, 0.42f, 1.0f};
inline const ImVec4 kRed  {1.00f, 0.42f, 0.42f, 1.0f};

inline bool isBuiltinPlugin(IEnginePlugin* p) { return pluginsview::isBuiltin(p); }
inline std::filesystem::path resolveKitModule(const ProjectContext& project,
                                              const std::string& module) {
    std::filesystem::path p(module);
    return p.is_absolute() ? p : (project.projectRoot / p);
}

// The contents of a plugin's own window: identity, live state, stock stats, and
// the plugin's own UI drawn through the engineUi* facade.
inline void drawPluginBody(IEnginePlugin* p, bool simulating) {
    ImGui::TextDisabled("%s  ·  v%s",
        isBuiltinPlugin(p) ? "Built-in" : "Kit / module", p->version());
    if (simulating) ImGui::TextColored(kGreen, ICON_FA_CIRCLE_PLAY " active");
    else            ImGui::TextDisabled("attached — idle (press Play to simulate)");

    if (auto* jolt = dynamic_cast<JoltPlugin*>(p)) {
        if (jolt->simulationActive()) ImGui::Text("Bodies:   %d", jolt->bodyCount());
        ImGui::Text("Fixed dt: %.0f Hz", 1.0f / JoltPlugin::fixedTimestep());
        ImGui::TextDisabled("Gravity: (0, -9.81, 0)");
    } else if (auto* lua = dynamic_cast<LuaScriptPlugin*>(p)) {
        ImGui::Text("VM:        %s", lua->vmRunning() ? "running" : "off");
        ImGui::Text("Instances: %d", lua->instanceCount());
        ImGui::TextDisabled("Scripts reload on each Play");
    }

    ImGui::Separator();
    p->onEditorUI();   // the plugin/kit's own controls (engineUi* facade), or nothing
}
} // namespace

// A separate dockable window per plugin. Close it with the title-bar [x];
// re-open from the Plug-in Manager's "Open" button.
inline void drawPluginWindows(PluginRegistry& plugins, PluginWindows& w, bool simulating) {
    for (auto& p : plugins.all()) {
        const std::string name = p->name();
        bool& open = w.open[name];        // default-inserts false the first time
        if (!open) continue;
        if (w.focus == name) { ImGui::SetNextWindowFocus(); w.focus.clear(); }
        ImGui::SetNextWindowSize(ImVec2(340, 260), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(name.c_str(), &open))
            drawPluginBody(p.get(), simulating);
        ImGui::End();
    }
}

inline void drawPluginsPanel(bool* open, bool* focus, PluginRegistry& plugins,
                             ProjectContext& project, const KitHost& kits,
                             bool simulating, PluginWindows& windows,
                             const std::function<void(const std::string&)>& loadKit,
                             const std::function<void(const std::string&)>& unloadKit) {
    if (open && !*open) return;
    if (focus && *focus) { ImGui::SetNextWindowFocus(); *focus = false; }
    // Title-bar [x] closes it; reopen from Plugins > Plugin Manager or View > Panels.
    if (!ImGui::Begin(ICON_FA_SCREWDRIVER_WRENCH " Plug-in Manager", open)) {
        ImGui::End();
        return;
    }

    // ── Running ── one row each; "Open" pops the plugin's own window ──────────
    ImGui::SeparatorText("Running");
    ImGui::TextDisabled("%zu attached%s", plugins.all().size(),
                        simulating ? "  ·  simulating" : "");
    for (auto& p : plugins.all()) {
        ImGui::PushID(p.get());
        if (ImGui::SmallButton("Open")) {
            windows.open[p->name()] = true;
            windows.focus = p->name();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(p->name());
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", isBuiltinPlugin(p.get()) ? "Built-in" : "Kit");
        if (simulating) { ImGui::SameLine(); ImGui::TextColored(kGreen, ICON_FA_CIRCLE_PLAY); }
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
        for (const pluginsview::KitRow& r : pluginsview::kitRows(project, &kits, simulating)) {
            ImGui::PushID((int)r.index);
            bool en = r.enabled;
            if (ImGui::Checkbox("##enabled", &en)) pluginsview::setKitEnabled(project, r.index, en);
            ImGui::SameLine();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::SameLine();
            using pluginsview::KitState;
            switch (r.state) {
            case KitState::Disabled:     ImGui::TextColored(kRed, ICON_FA_CIRCLE_XMARK " disabled"); break;
            case KitState::Loaded:       ImGui::TextColored(kGreen, ICON_FA_CIRCLE_CHECK " loaded"); break;
            case KitState::Unloaded:     ImGui::TextDisabled(ICON_FA_CIRCLE_XMARK " unloaded"); break;
            case KitState::Missing:      ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " missing"); break;
            case KitState::Failed:       ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " failed"); break;
            case KitState::PathNotFound: ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " path not found"); break;
            case KitState::LoadsAtPlay:  ImGui::TextDisabled(ICON_FA_CIRCLE_PLAY " loads at Play"); break;
            }
            if (r.canUnload) { ImGui::SameLine(); if (ImGui::SmallButton("Unload") && unloadKit) unloadKit(project.kits[r.index].name); }
            if (r.canLoad)   { ImGui::SameLine(); if (ImGui::SmallButton("Load")   && loadKit)   loadKit(project.kits[r.index].name); }
            ImGui::TextDisabled("      %s", r.path.string().c_str());
            if (!r.requiredKits.empty()) ImGui::TextDisabled("      requires: %s", r.requiredKits.c_str());
            if (!r.error.empty()) ImGui::TextColored(kRed, "      %s", r.error.c_str());
            ImGui::PopID();
        }
        if (simulating)
            ImGui::TextDisabled("Enable/disable applies on the next Play.");
    }

    ImGui::End();
}

// ── Kit-load failure modal ───────────────────────────────────────────────────
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
