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

    // ── The two edges ──────────────────────────────────────────────────────
    // Optional plugins broadcast before every registered plugin and after
    // every one, whatever order plugins were added or reloaded in. Not owned,
    // never attached or detached here, and not in all(). The C world API
    // (src/capi/eng_world.cpp) puts its systems' phases here (WO-073): UPDATE
    // before every kit's onFrame, LATE_UPDATE after all of them. nullptr
    // clears an edge.
    void setEdges(IEnginePlugin* first, IEnginePlugin* last) { m_first = first; m_last = last; }

    // ── Simulation broadcasts ──────────────────────────────────────────────
    // gameWorld is the freshly-populated game ECS — plugins may register
    // their own systems into it during onSimulationStart.
    void broadcastSimStart(flecs::world& gameWorld) {
        each("onSimulationStart", [&](IEnginePlugin& p) { p.onSimulationStart(gameWorld); });
    }

    void broadcastSimStop() {
        each("onSimulationStop", [&](IEnginePlugin& p) { p.onSimulationStop(); });
    }

    // Per-frame phases while SimState == Playing (Paused skips them). Called in
    // this order each frame, before rendering: pre-physics -> step -> post.
    void broadcastUpdate(flecs::world& gameWorld, float dt) {
        each("onUpdate", [&](IEnginePlugin& p) { p.onUpdate(gameWorld, dt); });
    }
    // RENDER-RATE (real dt, after the fixed sim steps): presentation-only.
    void broadcastFrame(flecs::world& gameWorld, float dt) {
        each("onFrame", [&](IEnginePlugin& p) { p.onFrame(gameWorld, dt); });
    }
    void broadcastPhysicsStep(flecs::world& gameWorld, float dt) {
        each("onPhysicsStep", [&](IEnginePlugin& p) { p.onPhysicsStep(gameWorld, dt); });
    }
    void broadcastPostPhysics(flecs::world& gameWorld) {
        each("onPostPhysics", [&](IEnginePlugin& p) { p.onPostPhysics(gameWorld); });
    }

    // Editor UI is NOT broadcast here — a host walks all() and calls
    // onEditorUI() on each plugin, which draws through the engineUi* facade.
    const std::vector<std::shared_ptr<IEnginePlugin>>& all() const {
        return m_plugins;
    }

private:
    // The first edge, every plugin in order, then the last edge.
    template <typename Fn>
    void each(const char* phase, Fn&& fn) {
        if (m_first) guarded(*m_first, phase, [&] { fn(*m_first); });
        for (auto& p : m_plugins)
            guarded(*p, phase, [&] { fn(*p); });
        if (m_last) guarded(*m_last, phase, [&] { fn(*m_last); });
    }

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
    IEnginePlugin* m_first = nullptr;
    IEnginePlugin* m_last  = nullptr;
};
