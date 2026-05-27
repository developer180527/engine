#pragma once
#include "engine/plugin.h"
#include "engine/logger.h"
#include <imgui.h>

// ── NullScriptPlugin ───────────────────────────────────────────────────────
// Placeholder scripting backend. Proves the scripting plugin slot before
// LuaJIT is integrated. All methods are no-ops except logging and UI.
class NullScriptPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Scripting"; }
    const char* version() const override { return "0.0.0-null"; }

    void onAttach(EngineContext&) override {
        LOG_INFO("Script", "Null backend — no scripting");
    }
    void onDetach() override {}

    void onSimulationStart(flecs::world&) override {
        LOG_INFO("Script", "Simulation start (null — no scripts executed)");
    }
    void onSimulationStop() override {}
    void onUpdate(flecs::world&, float) override {}

    void onEditorUI() override {
        ImGui::TextDisabled("Backend:  Null (no scripting)");
        ImGui::TextDisabled("Next:     LuaJIT + FFI");
        ImGui::Spacing();
        ImGui::TextDisabled("Pipeline (planned):");
        ImGui::TextDisabled("  Lua systems registered into game ECS");
        ImGui::TextDisabled("  FFI typedefs from flecs meta schemas");
        ImGui::TextDisabled("  Zero per-entity C boundary crossings");
        ImGui::Spacing();
        ImGui::TextDisabled("Reserved component:");
        ImGui::TextDisabled("  ScriptComponent — script asset path");
    }
};
