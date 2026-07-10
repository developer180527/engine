#pragma once
// ── KitHost — lazy dynamic loading of a project's plugged-in kits ────────────
// Kits (FPS controller, IK, water, ...) are reusable C++ gameplay systems built
// ON the engine, each its own repo, listed in the project manifest. They are
// SIMULATION plugins — their work only happens during Play — so the runtime
// loads them LAZILY at simulation start and unloads them at stop:
//
//   startSimulation()  → KitHost::start()   dlopen + register + onAttach,
//                          then the runtime's broadcastSimStart fires their
//                          onSimulationStart along with every other plugin.
//   tickSimulation()   → KitHost::poll()    hot-reload any kit whose .so
//                          changed on disk, live against the running world.
//   stopSimulation()   → KitHost::stop()    onDetach + unregister + dlclose,
//                          after the runtime's broadcastSimStop.
//
// Individual kits can also be unloaded/loaded mid-play (unloadOne/loadOne —
// the Plug-in Manager buttons); a mid-play load runs onSimulationStart itself
// since the broadcast already happened.
//
// Load ORDER: manifest "requires" lists kits that must load before a kit
// (service-publish ordering — kits never link each other's code). start()
// topologically sorts enabled kits; a cycle logs an error and falls back to
// manifest order for the kits involved.
//
// While not playing, no kit .so is held open — rebuild kits freely.
#include "runtime/module_loader.h"
#include "runtime/plugin_registry.h"
#include "runtime/runtime_context.h"
#include "project/project_context.h"
#include "core/logger.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class KitHost {
public:
    // Per-manifest-kit outcome of the last start() — the truth the editor shows
    // instead of guessing from "is the sim running". Lives only while playing.
    struct KitStatus {
        enum class State { Loaded, FileNotFound, LoadFailed, Unloaded };
        std::string           name;
        std::filesystem::path resolvedPath;   // where we actually looked
        State                 state = State::LoadFailed;
        std::string           message;        // human-readable reason / version
    };

    // Load every enabled kit from the manifest (dependency-ordered) and attach
    // it. Call BEFORE broadcastSimStart so the broadcast includes them.
    void start(const ProjectContext& project, PluginRegistry& reg, RuntimeContext& ctx) {
        m_status.clear();
        for (const ProjectContext::Kit* k : sortedByRequires(project))
            loadKit(*k, project, reg, ctx, nullptr);   // simStart via broadcast
    }

    // Truthful load results for the current play session (empty while idle).
    const std::vector<KitStatus>& status() const { return m_status; }
    bool anyFailed() const {
        for (const auto& s : m_status)
            if (s.state == KitStatus::State::FileNotFound ||
                s.state == KitStatus::State::LoadFailed) return true;
        return false;
    }
    bool isLoaded(const std::string& name) const {
        for (const auto& e : m_loaded) if (e->name == name) return true;
        return false;
    }

    // Load a single manifest kit mid-play (Plug-in Manager "Load"). Runs its
    // onSimulationStart directly — the broadcast already happened.
    bool loadOne(const std::string& name, const ProjectContext& project,
                 PluginRegistry& reg, RuntimeContext& ctx, flecs::world& world) {
        if (isLoaded(name)) return true;
        for (const auto& k : project.kits)
            if (k.name == name)
                return loadKit(k, project, reg, ctx, &world);
        LOG_ERROR("Kit", "loadOne: '%s' is not in the manifest", name.c_str());
        return false;
    }

    // Unload a single kit mid-play (Plug-in Manager "Unload"): simStop +
    // detach + unregister + dlclose. Its state simply stops advancing; other
    // kits keep running (kits never call each other, so nothing dangles).
    bool unloadOne(const std::string& name, PluginRegistry& reg) {
        for (size_t i = 0; i < m_loaded.size(); ++i) {
            if (m_loaded[i]->name != name) continue;
            Entry& e = *m_loaded[i];
            e.plugin->onSimulationStop();
            e.plugin->onDetach();
            reg.remove(e.plugin.get());
            e.plugin.reset();
            e.lib.unload();
            setStatus(name, m_loaded[i]->path, KitStatus::State::Unloaded,
                      "unloaded (Plug-in Manager)");
            m_loaded.erase(m_loaded.begin() + (long)i);
            LOG_INFO("Kit", "unloaded '%s'", name.c_str());
            return true;
        }
        return false;
    }

    // Detach + unregister + dlclose every kit. Call AFTER broadcastSimStop
    // (onSimulationStop has already fired on them). The shared_ptr must be
    // released before unload() — the table the adapter points at dies there.
    void stop(PluginRegistry& reg) {
        for (auto& e : m_loaded) {
            e->plugin->onDetach();
            reg.remove(e->plugin.get());
            e->plugin.reset();
            e->lib.unload();
        }
        m_loaded.clear();
        m_status.clear();   // no runtime status while idle
    }

    // Hot-reload: swap any kit whose .so changed on disk, live against the
    // running sim world. Call early in tickSimulation, before the broadcasts,
    // so the registry is stable while they iterate.
    void poll(float dt, PluginRegistry& reg, RuntimeContext& ctx, flecs::world& world) {
        for (size_t i = 0; i < m_loaded.size(); /* stepped below */) {
            auto& e = m_loaded[i];
            if (!e->watcher.changed(dt)) { ++i; continue; }

            e->plugin->onSimulationStop();        // stop old code
            e->plugin->onDetach();
            reg.remove(e->plugin.get());
            e->plugin.reset();
            e->lib.unload();

            if (!e->lib.load(e->path)) {          // bring up new code
                // Audit C.2: the old `continue` left this Entry in m_loaded
                // with plugin == nullptr — stop() then null-derefed on the
                // next Stop, isLoaded() reported the dead kit healthy (so
                // loadOne() no-oped forever), and the status UI froze on
                // "Loaded". The kit is fully torn down at this point, so
                // make that the recorded reality: erase the entry, surface
                // LoadFailed. A fixed rebuild comes back via loadOne().
                LOG_ERROR("Kit", "reload failed: '%s' — kit unloaded "
                          "(fix the module, then load it again)",
                          e->name.c_str());
                setStatus(e->name, e->path, KitStatus::State::LoadFailed,
                          "reload failed — ABI mismatch or bad module "
                          "(see console); kit unloaded");
                m_loaded.erase(m_loaded.begin() + (long)i);
                continue;                          // same index: next entry
            }
            e->plugin = e->lib.plugin();
            reg.add(e->plugin);
            e->plugin->onAttach(ctx);
            e->plugin->onSimulationStart(world);  // no broadcast mid-sim — start it here
            LOG_SUCCESS("Kit", "reloaded '%s'", e->name.c_str());
            setStatus(e->name, e->path, KitStatus::State::Loaded,
                      e->plugin->version());
            ++i;
        }
    }

    bool empty() const { return m_loaded.empty(); }

private:
    struct Entry {
        Entry(std::string n, std::filesystem::path p)
            : name(std::move(n)), path(std::move(p)), watcher(path) {}
        std::string                    name;
        std::filesystem::path          path;
        modload::ModuleLibrary         lib;
        modload::ModuleWatcher         watcher;
        std::shared_ptr<IEnginePlugin> plugin;
    };

    // Absolute manifest paths pass through; relative ones resolve against the
    // project root (kits typically live under <engine>/Kits/<name>).
    static std::filesystem::path resolve(const ProjectContext& project,
                                         const std::string& module) {
        std::filesystem::path p(module);
        return p.is_absolute() ? p : (project.projectRoot / p);
    }

    void setStatus(const std::string& name, const std::filesystem::path& path,
                   KitStatus::State st, std::string msg) {
        for (auto& s : m_status)
            if (s.name == name) { s.resolvedPath = path; s.state = st;
                                  s.message = std::move(msg); return; }
        m_status.push_back({name, path, st, std::move(msg)});
    }

    // Shared load path. simWorld == null on start() (broadcast fires simStart);
    // non-null on a mid-play loadOne (we fire it ourselves).
    bool loadKit(const ProjectContext::Kit& k, const ProjectContext& project,
                 PluginRegistry& reg, RuntimeContext& ctx, flecs::world* simWorld) {
        if (!k.enabled) return false;
        std::filesystem::path path = resolve(project, k.module);
        if (!std::filesystem::exists(path)) {
            LOG_ERROR("Kit", "'%s' module not found: %s",
                      k.name.c_str(), path.string().c_str());
            setStatus(k.name, path, KitStatus::State::FileNotFound,
                      "module file not found");
            return false;
        }
        auto e = std::make_unique<Entry>(k.name, path);
        if (!e->lib.load(path)) {
            LOG_ERROR("Kit", "failed to load '%s'", k.name.c_str());
            setStatus(k.name, path, KitStatus::State::LoadFailed,
                      "load failed — ABI mismatch or bad module (see console)");
            return false;
        }
        e->plugin = e->lib.plugin();
        reg.add(e->plugin);
        e->plugin->onAttach(ctx);
        if (simWorld) e->plugin->onSimulationStart(*simWorld);
        LOG_SUCCESS("Kit", "loaded '%s' %s", e->plugin->name(), e->plugin->version());
        setStatus(k.name, path, KitStatus::State::Loaded, e->plugin->version());
        m_loaded.push_back(std::move(e));
        return true;
    }

    // Kahn's algorithm over the enabled kits' "requires" edges (dep -> kit).
    // Unknown/disabled requirements warn and are ignored (ordering-only
    // semantics: the dependent still loads). A cycle logs an error; the kits
    // stuck in it are appended in manifest order.
    static std::vector<const ProjectContext::Kit*>
    sortedByRequires(const ProjectContext& project) {
        std::vector<const ProjectContext::Kit*> enabled;
        std::unordered_map<std::string, size_t> index;   // name -> enabled[] slot
        for (const auto& k : project.kits)
            if (k.enabled) { index[k.name] = enabled.size(); enabled.push_back(&k); }

        const size_t n = enabled.size();
        std::vector<int>                 indeg(n, 0);
        std::vector<std::vector<size_t>> out(n);         // dep -> dependents
        for (size_t i = 0; i < n; ++i) {
            for (const auto& dep : enabled[i]->requiresKits) {
                auto it = index.find(dep);
                if (it == index.end()) {
                    LOG_WARN("Kit", "'%s' requires '%s', which is not an enabled "
                             "manifest kit — ignoring for ordering",
                             enabled[i]->name.c_str(), dep.c_str());
                    continue;
                }
                out[it->second].push_back(i);
                ++indeg[i];
            }
        }

        std::vector<const ProjectContext::Kit*> sorted;
        std::vector<size_t> ready;
        for (size_t i = 0; i < n; ++i) if (indeg[i] == 0) ready.push_back(i);
        std::vector<bool> emitted(n, false);
        while (!ready.empty()) {
            // Pop the lowest index so ties keep manifest order (stable).
            size_t best = 0;
            for (size_t j = 1; j < ready.size(); ++j) if (ready[j] < ready[best]) best = j;
            size_t i = ready[best];
            ready.erase(ready.begin() + (long)best);
            emitted[i] = true;
            sorted.push_back(enabled[i]);
            for (size_t d : out[i]) if (--indeg[d] == 0) ready.push_back(d);
        }
        if (sorted.size() != n) {
            LOG_ERROR("Kit", "'requires' cycle detected — the kits involved "
                      "load in manifest order");
            for (size_t i = 0; i < n; ++i)
                if (!emitted[i]) sorted.push_back(enabled[i]);
        }
        return sorted;
    }

    std::vector<std::unique_ptr<Entry>> m_loaded;
    std::vector<KitStatus>              m_status;
};
