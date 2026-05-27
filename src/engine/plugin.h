#pragma once
#include <flecs.h>

// Forward — plugins receive EngineContext on attach for accessing
// asset storage, ECS world, project settings etc.
struct EngineContext;

// ── IEnginePlugin ──────────────────────────────────────────────────────────
// All engine subsystems (physics, scripting, audio, networking) implement
// this interface. The engine calls each lifecycle method at the right moment;
// plugins are completely decoupled from each other and from editor code.
//
// Lifecycle:
//   onAttach        — registered with engine (editor startup)
//   onDetach        — unregistered (editor shutdown)
//   onSimulationStart — Play pressed, game world freshly populated
//   onSimulationStop  — Stop pressed, game world about to be destroyed
//   onUpdate          — called every frame during Playing (not Paused)
//   onEditorUI        — draw plugin-specific ImGui widgets (settings, stats)
class IEnginePlugin {
public:
    virtual ~IEnginePlugin() = default;

    virtual const char* name()    const = 0;
    virtual const char* version() const = 0;

    // Editor lifecycle
    virtual void onAttach(EngineContext& ctx) = 0;
    virtual void onDetach()                   = 0;

    // Simulation lifecycle — game world is ONLY valid between Start and Stop
    virtual void onSimulationStart(flecs::world& gameWorld) = 0;
    virtual void onSimulationStop()                         = 0;

    // Per-frame update — only called when SimState == Playing
    virtual void onUpdate(flecs::world& gameWorld, float dt) = 0;

    // Editor UI — called inside an existing ImGui scope (Plugins menu etc.)
    virtual void onEditorUI() = 0;
};
