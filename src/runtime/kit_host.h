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
// While not playing, no kit .so is held open — rebuild kits freely. The
// manifest itself is parsed eagerly (ProjectContext); only the code loads lazy.
#include "runtime/module_loader.h"
#include "runtime/plugin_registry.h"
#include "runtime/runtime_context.h"
#include "project/project_context.h"
#include "core/logger.h"

#include <filesystem>
#include <memory>
#include <vector>

class KitHost {
public:
    // Per-manifest-kit outcome of the last start() — the truth the editor shows
    // instead of guessing from "is the sim running". Lives only while playing.
    struct KitStatus {
        enum class State { Loaded, FileNotFound, LoadFailed };
        std::string           name;
        std::filesystem::path resolvedPath;   // where we actually looked
        State                 state = State::LoadFailed;
        std::string           message;        // human-readable reason / version
    };

    // Load every enabled kit from the manifest and attach it. Call BEFORE
    // broadcastSimStart so the broadcast includes the freshly-attached kits.
    void start(const ProjectContext& project, PluginRegistry& reg, RuntimeContext& ctx) {
        m_status.clear();
        for (const auto& k : project.kits) {
            if (!k.enabled) continue;
            std::filesystem::path path = resolve(project, k.module);
            if (!std::filesystem::exists(path)) {
                LOG_ERROR("Kit", "'%s' module not found: %s",
                          k.name.c_str(), path.string().c_str());
                m_status.push_back({k.name, path, KitStatus::State::FileNotFound,
                                    "module file not found"});
                continue;
            }
            auto e = std::make_unique<Entry>(k.name, path);
            if (!e->lib.load(path)) {
                LOG_ERROR("Kit", "failed to load '%s'", k.name.c_str());
                m_status.push_back({k.name, path, KitStatus::State::LoadFailed,
                                    "load failed — ABI mismatch or bad module (see console)"});
                continue;
            }
            e->plugin = e->lib.plugin();
            reg.add(e->plugin);
            e->plugin->onAttach(ctx);
            LOG_SUCCESS("Kit", "loaded '%s' %s", e->plugin->name(), e->plugin->version());
            m_status.push_back({k.name, path, KitStatus::State::Loaded,
                                e->plugin->version()});
            m_loaded.push_back(std::move(e));
        }
    }

    // Truthful load results for the current play session (empty while idle).
    const std::vector<KitStatus>& status() const { return m_status; }
    bool anyFailed() const {
        for (const auto& s : m_status)
            if (s.state != KitStatus::State::Loaded) return true;
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
        for (auto& e : m_loaded) {
            if (!e->watcher.changed(dt)) continue;

            e->plugin->onSimulationStop();        // stop old code
            e->plugin->onDetach();
            reg.remove(e->plugin.get());
            e->plugin.reset();
            e->lib.unload();

            if (!e->lib.load(e->path)) {          // bring up new code
                LOG_ERROR("Kit", "reload failed: '%s'", e->name.c_str());
                continue;
            }
            e->plugin = e->lib.plugin();
            reg.add(e->plugin);
            e->plugin->onAttach(ctx);
            e->plugin->onSimulationStart(world);  // no broadcast mid-sim — start it here
            LOG_SUCCESS("Kit", "reloaded '%s'", e->name.c_str());
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
    // project root (kits typically sit in sibling repos, e.g.
    // "../FPSPlayerControllerKit/build/libfps_controller_kit_module.so").
    static std::filesystem::path resolve(const ProjectContext& project,
                                         const std::string& module) {
        std::filesystem::path p(module);
        return p.is_absolute() ? p : (project.projectRoot / p);
    }

    std::vector<std::unique_ptr<Entry>> m_loaded;
    std::vector<KitStatus>              m_status;
};
