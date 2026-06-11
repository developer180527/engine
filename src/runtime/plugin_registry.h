#pragma once
#include <algorithm>
#include <exception>
#include <vector>
#include <memory>
#include "runtime/plugin.h"
#include "core/logger.h"

// ── PluginRegistry ─────────────────────────────────────────────────────────
// Owns all registered plugins and fans out lifecycle events to each one.
// Lives in EngineRuntime — plugins are a runtime concept; standalone games
// register them exactly like the editor does. Plugins are registered at
// startup before the first frame and remain attached until shutdown.
class PluginRegistry {
public:
    // Register a plugin. Must be called before attachAll().
    void add(std::shared_ptr<IEnginePlugin> plugin) {
        m_plugins.push_back(std::move(plugin));
    }

    // Remove a plugin from the broadcast list WITHOUT calling its lifecycle —
    // the caller orchestrates detach. Used by hot reload (engine_host), which
    // must control the exact stop/detach/unload ordering.
    void remove(IEnginePlugin* p) {
        m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
            [p](const std::shared_ptr<IEnginePlugin>& sp) { return sp.get() == p; }),
            m_plugins.end());
    }

    // Call after the runtime context is ready (EngineRuntime::attachPlugins).
    void attachAll(RuntimeContext& ctx) {
        for (auto& p : m_plugins) {
            guarded(*p, "onAttach", [&] { p->onAttach(ctx); });
            LOG_INFO("Plugin", "Attached: %s %s", p->name(), p->version());
        }
    }

    // Call at shutdown.
    void detachAll() {
        for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it)
            guarded(**it, "onDetach", [&] { (*it)->onDetach(); });
    }

    // ── Simulation broadcasts ──────────────────────────────────────────────
    // gameWorld is the freshly-populated game ECS — plugins may register
    // their own systems into it during onSimulationStart.
    void broadcastSimStart(flecs::world& gameWorld) {
        for (auto& p : m_plugins)
            guarded(*p, "onSimulationStart", [&] { p->onSimulationStart(gameWorld); });
    }

    void broadcastSimStop() {
        for (auto& p : m_plugins)
            guarded(*p, "onSimulationStop", [&] { p->onSimulationStop(); });
    }

    // Per-frame phases while SimState == Playing (Paused skips them). Called in
    // this order each frame, before rendering: pre-physics -> step -> post.
    void broadcastUpdate(flecs::world& gameWorld, float dt) {
        for (auto& p : m_plugins)
            guarded(*p, "onUpdate", [&] { p->onUpdate(gameWorld, dt); });
    }
    void broadcastPhysicsStep(flecs::world& gameWorld, float dt) {
        for (auto& p : m_plugins)
            guarded(*p, "onPhysicsStep", [&] { p->onPhysicsStep(gameWorld, dt); });
    }
    void broadcastPostPhysics(flecs::world& gameWorld) {
        for (auto& p : m_plugins)
            guarded(*p, "onPostPhysics", [&] { p->onPostPhysics(gameWorld); });
    }

    // Editor UI is NOT broadcast here — the editor walks all() and
    // dynamic_casts each plugin to IEditorPlugin (editor/editor_plugin.h).
    const std::vector<std::shared_ptr<IEnginePlugin>>& all() const {
        return m_plugins;
    }

private:
    // Exception fence: one plugin throwing must not take down the frame or
    // skip its peers. Honest limit — segfaults/aborts remain uncatchable;
    // this guards the recoverable failure class only.
    template <typename Fn>
    static void guarded(IEnginePlugin& p, const char* phase, Fn&& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            LOG_ERROR("Plugin", "%s threw in %s: %s", p.name(), phase, e.what());
        } catch (...) {
            LOG_ERROR("Plugin", "%s threw an unknown exception in %s",
                      p.name(), phase);
        }
    }

    std::vector<std::shared_ptr<IEnginePlugin>> m_plugins;
};
