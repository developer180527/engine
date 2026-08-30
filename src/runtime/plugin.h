#pragma once
#include <flecs.h>

// Forward — plugins receive RuntimeContext on attach for accessing
// asset storage, ECS world, project settings etc. Editor-free: a
// standalone game attaches plugins exactly the same way.
struct RuntimeContext;

// ── IEnginePlugin ──────────────────────────────────────────────────────────
// All engine subsystems (physics, scripting, audio, networking) implement
// this interface. The engine calls each lifecycle method at the right moment;
// plugins are completely decoupled from each other and from editor code.
//
// Plugins that also want to draw editor UI override onEditorUI() below and
// draw through the engineUi* facade (engine/engine_api.h) — five widgets over
// C types, routed to whatever backend the host registered. The runtime never
// calls UI methods, and a plugin never sees the UI toolkit.
//
// Lifecycle:
//   onAttach        — registered with engine (startup)
//   onDetach        — unregistered (shutdown)
//   onSimulationStart — Play pressed / game boot, game world freshly populated
//   onSimulationStop  — Stop pressed / game exit, game world about to be destroyed
//   onUpdate          — called every frame while simulating (not paused)
class IEnginePlugin {
public:
    virtual ~IEnginePlugin() = default;

    virtual const char* name()    const = 0;
    virtual const char* version() const = 0;

    // Engine lifecycle
    virtual void onAttach(RuntimeContext& ctx) = 0;
    virtual void onDetach()                    = 0;

    // Simulation lifecycle — game world is ONLY valid between Start and Stop
    virtual void onSimulationStart(flecs::world& gameWorld) = 0;
    virtual void onSimulationStop()                         = 0;

    // Per-frame phases, only while simulating, called in THIS order
    // each frame BEFORE rendering. All default-empty — implement only what you need:
    //   onUpdate      — pre-physics: gameplay/scripts read input + set intent
    //   onPhysicsStep — advance the simulation (applies the intent above)
    //   onPostPhysics — after the step: dispatch collision events / late update
    virtual void onUpdate     (flecs::world& /*gameWorld*/, float /*dt*/) {}
    virtual void onPhysicsStep(flecs::world& /*gameWorld*/, float /*dt*/) {}
    virtual void onPostPhysics(flecs::world& /*gameWorld*/)               {}
    // RENDER-RATE: once per rendered frame, after the fixed sim steps —
    // presentation-only work (late-latched camera look, view effects).
    // Gameplay state mutation belongs in onUpdate (fixed dt), never here.
    virtual void onFrame      (flecs::world& /*gameWorld*/, float /*dt*/) {}

    // Optional editor panel: draw with the engineUi* facade (NOT ImGui). The
    // editor calls this for each plugin; with no UI surface the calls no-op.
    // Lets a kit own a tuning/debug UI without linking any editor/ImGui code.
    virtual void onEditorUI() {}
};
