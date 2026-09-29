#pragma once
// ── Plug-in Manager model — plugins and kits as rows, without a GUI ──────────
// What the Plug-in Manager shows: the running plugins (built-in or kit), each
// one's detail lines, and the project's kits with their state — resolved from
// the kit host while playing, from the file system otherwise — plus what the
// user may do with each (enable, load, unload). The ImGui panel and the libgui
// front end draw the same rows. A plugin's own controls (onEditorUI, the
// engineUi* facade) are not here: that facade renders through ImGui today.
#include "editor/core/color.h"
#include "plugins/audio_plugin.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "project/project_context.h"
#include "runtime/kit_host.h"
#include "runtime/plugin_registry.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace pluginsview {

inline const edui::Color kGreen{0.30f, 1.00f, 0.42f, 1.0f};
inline const edui::Color kRed  {1.00f, 0.42f, 0.42f, 1.0f};

inline bool isBuiltin(IEnginePlugin* p) {
    return dynamic_cast<JoltPlugin*>(p) || dynamic_cast<LuaScriptPlugin*>(p)
        || dynamic_cast<AudioPlugin*>(p);
}

struct PluginRow { IEnginePlugin* plugin; std::string name; bool builtin; std::string version; };

inline std::vector<PluginRow> pluginRows(PluginRegistry& reg) {
    std::vector<PluginRow> out;
    for (auto& p : reg.all())
        out.push_back({p.get(), p->name(), isBuiltin(p.get()), p->version()});
    return out;
}

// A plugin's status lines for its detail view.
inline std::vector<std::string> pluginDetails(IEnginePlugin* p, bool simulating) {
    std::vector<std::string> out;
    char buf[128];
    out.push_back(std::string(isBuiltin(p) ? "Built-in" : "Kit / module") + "  ·  v" + p->version());
    out.push_back(simulating ? "active" : "attached — idle (press Play to simulate)");
    if (auto* jolt = dynamic_cast<JoltPlugin*>(p)) {
        if (jolt->simulationActive()) { std::snprintf(buf, sizeof buf, "Bodies:   %d", jolt->bodyCount()); out.push_back(buf); }
        std::snprintf(buf, sizeof buf, "Fixed dt: %.0f Hz", 1.0f / JoltPlugin::fixedTimestep()); out.push_back(buf);
        out.push_back("Gravity: (0, -9.81, 0)");
    } else if (auto* lua = dynamic_cast<LuaScriptPlugin*>(p)) {
        out.push_back(std::string("VM:        ") + (lua->vmRunning() ? "running" : "off"));
        std::snprintf(buf, sizeof buf, "Instances: %d", lua->instanceCount()); out.push_back(buf);
        out.push_back("Scripts reload on each Play");
    }
    return out;
}

enum class KitState { Disabled, Loaded, Unloaded, Missing, Failed, PathNotFound, LoadsAtPlay };

inline const char* kitStateText(KitState s) {
    switch (s) {
    case KitState::Disabled:     return "disabled";
    case KitState::Loaded:       return "loaded";
    case KitState::Unloaded:     return "unloaded";
    case KitState::Missing:      return "missing";
    case KitState::Failed:       return "failed";
    case KitState::PathNotFound: return "path not found";
    case KitState::LoadsAtPlay:  return "loads at Play";
    }
    return "?";
}
inline bool kitStateIsError(KitState s) {
    return s == KitState::Disabled || s == KitState::Missing || s == KitState::Failed
        || s == KitState::PathNotFound;
}

struct KitRow {
    size_t                index = 0;         // into project.kits
    std::string           name;
    std::filesystem::path path;               // the module, resolved
    bool                  enabled = false;
    KitState              state = KitState::LoadsAtPlay;
    std::string           error;              // why, when there is a why
    std::string           requiredKits;           // "a, b"
    bool                  canLoad = false, canUnload = false;   // only while playing
};

inline std::filesystem::path resolveKitModule(const ProjectContext& project, const std::string& module) {
    std::filesystem::path p(module);
    return p.is_absolute() ? p : (project.projectRoot / p);
}

inline std::vector<KitRow> kitRows(const ProjectContext& project, const KitHost* kits, bool simulating) {
    std::vector<KitRow> out;
    for (size_t i = 0; i < project.kits.size(); ++i) {
        const auto& k = project.kits[i];
        KitRow r;
        r.index = i;
        r.name = k.name.empty() ? k.module : k.name;
        r.path = resolveKitModule(project, k.module);
        r.enabled = k.enabled;
        for (const auto& d : k.requiresKits) { if (!r.requiredKits.empty()) r.requiredKits += ", "; r.requiredKits += d; }

        const KitHost::KitStatus* st = nullptr;   // truth while playing
        if (kits) for (const auto& s : kits->status()) if (s.name == k.name) { st = &s; break; }
        const bool exists = std::filesystem::exists(r.path);
        if (!k.enabled) {
            r.state = KitState::Disabled;
        } else if (st) {
            using S = KitHost::KitStatus::State;
            switch (st->state) {
            case S::Loaded:       r.state = KitState::Loaded;   break;
            case S::Unloaded:     r.state = KitState::Unloaded; break;
            case S::FileNotFound: r.state = KitState::Missing;  r.error = st->message; break;
            case S::LoadFailed:   r.state = KitState::Failed;   r.error = st->message; break;
            }
        } else if (!exists) {
            r.state = KitState::PathNotFound;
            r.error = "module file does not exist at this path";
        } else {
            r.state = KitState::LoadsAtPlay;
        }
        if (simulating && k.enabled && kits) {
            r.canUnload = kits->isLoaded(k.name);
            r.canLoad   = !r.canUnload && exists;
        }
        out.push_back(std::move(r));
    }
    return out;
}

// Enabling/disabling writes project.json at once; it applies on the next Play.
inline void setKitEnabled(ProjectContext& project, size_t index, bool enabled) {
    if (index >= project.kits.size() || project.kits[index].enabled == enabled) return;
    project.kits[index].enabled = enabled;
    project.save();
}

}  // namespace pluginsview
