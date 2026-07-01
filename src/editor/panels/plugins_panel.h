#pragma once
#include <imgui.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include "editor/editor_icons.h"
#include "runtime/plugin_registry.h"
#include "runtime/kit_host.h"
#include "project/project_context.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"

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

inline bool isBuiltinPlugin(IEnginePlugin* p) {
    return dynamic_cast<JoltPlugin*>(p)
        || dynamic_cast<LuaScriptPlugin*>(p)
        || dynamic_cast<AudioPlugin*>(p);
}
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
                             bool simulating, PluginWindows& windows) {
    if (open && !*open) return;
    if (focus && *focus) { ImGui::SetNextWindowFocus(); *focus = false; }
    // No close button on the title bar — visibility is the Plugins menu's job.
    if (!ImGui::Begin(ICON_FA_SCREWDRIVER_WRENCH " Plug-in Manager")) {
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
        bool dirty = false;
        for (auto& k : project.kits) {
            ImGui::PushID(&k);

            const KitHost::KitStatus* st = nullptr;   // truth while playing
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
            } else if (st) {
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
            } else if (!std::filesystem::exists(full)) {
                ImGui::TextColored(kRed, ICON_FA_TRIANGLE_EXCLAMATION " path not found");
                errLine = "module file does not exist at this path";
            } else {
                ImGui::TextDisabled(ICON_FA_CIRCLE_PLAY " loads at Play");
            }

            ImGui::TextDisabled("      %s", full.string().c_str());
            if (errLine) ImGui::TextColored(kRed, "      %s", errLine);
            ImGui::PopID();
        }
        if (dirty) project.save();
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
