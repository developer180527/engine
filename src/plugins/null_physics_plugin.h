#pragma once
#include "runtime/plugin.h"
#include "runtime/logger.h"
#include <imgui.h>

// ── NullPhysicsPlugin ──────────────────────────────────────────────────────
// Placeholder physics backend. Satisfies the physics plugin slot so the
// architecture is proven before Jolt is integrated. Logs lifecycle events,
// draws a stub UI, does zero simulation work.
class NullPhysicsPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Physics"; }
    const char* version() const override { return "0.0.0-null"; }

    void onAttach(EngineContext&) override {
        LOG_INFO("Physics", "Null backend — no simulation");
    }
    void onDetach() override {}

    void onSimulationStart(flecs::world&) override {
        LOG_INFO("Physics", "Simulation start (null — no rigid bodies stepped)");
    }
    void onSimulationStop() override {
        LOG_INFO("Physics", "Simulation stop");
    }

    // Null backend does zero work per frame
    void onUpdate(flecs::world&, float) override {}

    void onEditorUI() override {
        ImGui::TextDisabled("Backend:  Null (no simulation)");
        ImGui::TextDisabled("Next:     Jolt Physics");
        ImGui::Spacing();
        ImGui::TextDisabled("Reserved components:");
        ImGui::TextDisabled("  RigidBody  — mass, restitution, friction");
        ImGui::TextDisabled("  Collider   — shape, extents, is_trigger");
    }
};
