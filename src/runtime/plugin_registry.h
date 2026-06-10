#pragma once
#include <vector>
#include <memory>
#include "runtime/plugin.h"
#include "runtime/logger.h"

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

    // Call after the runtime context is ready (EngineRuntime::attachPlugins).
    void attachAll(RuntimeContext& ctx) {
        for (auto& p : m_plugins) {
            p->onAttach(ctx);
            LOG_INFO("Plugin", "Attached: %s %s", p->name(), p->version());
        }
    }

    // Call at shutdown.
    void detachAll() {
        for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it)
            (*it)->onDetach();
    }

    // ── Simulation broadcasts ──────────────────────────────────────────────
    // gameWorld is the freshly-populated game ECS — plugins may register
    // their own systems into it during onSimulationStart.
    void broadcastSimStart(flecs::world& gameWorld) {
        for (auto& p : m_plugins) p->onSimulationStart(gameWorld);
    }

    void broadcastSimStop() {
        for (auto& p : m_plugins) p->onSimulationStop();
    }

    // Per-frame phases while SimState == Playing (Paused skips them). Called in
    // this order each frame, before rendering: pre-physics -> step -> post.
    void broadcastUpdate(flecs::world& gameWorld, float dt) {
        for (auto& p : m_plugins) p->onUpdate(gameWorld, dt);
    }
    void broadcastPhysicsStep(flecs::world& gameWorld, float dt) {
        for (auto& p : m_plugins) p->onPhysicsStep(gameWorld, dt);
    }
    void broadcastPostPhysics(flecs::world& gameWorld) {
        for (auto& p : m_plugins) p->onPostPhysics(gameWorld);
    }

    // Editor UI is NOT broadcast here — the editor walks all() and
    // dynamic_casts each plugin to IEditorPlugin (editor/editor_plugin.h).
    const std::vector<std::shared_ptr<IEnginePlugin>>& all() const {
        return m_plugins;
    }

private:
    std::vector<std::shared_ptr<IEnginePlugin>> m_plugins;
};
