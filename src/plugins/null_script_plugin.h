#pragma once
#include "runtime/plugin.h"
#include "runtime/logger.h"

// ── NullScriptPlugin ───────────────────────────────────────────────────────
// Placeholder scripting backend. Proves the scripting plugin slot before
// LuaJIT is integrated. All methods are no-ops except logging and UI.
class NullScriptPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Scripting"; }
    const char* version() const override { return "0.0.0-null"; }

    void onAttach(RuntimeContext&) override {
        LOG_INFO("Script", "Null backend — no scripting");
    }
    void onDetach() override {}

    void onSimulationStart(flecs::world&) override {
        LOG_INFO("Script", "Simulation start (null — no scripts executed)");
    }
    void onSimulationStop() override {}
    void onUpdate(flecs::world&, float) override {}
};
