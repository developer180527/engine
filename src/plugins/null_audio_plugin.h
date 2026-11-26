#pragma once
#include "runtime/plugin.h"
#include "core/logger.h"

// ── NullAudioPlugin — the `none` audio provider ──────────────────────────────
// Chosen by name ("audio": "none"), never as a fallback (WO-076). It publishes
// no audio service, so a script that reaches for audio gets ScriptHost's
// one-time warning, exactly as before this provider existed.
class NullAudioPlugin final : public IEnginePlugin {
public:
    const char* name()    const override { return "Audio"; }
    const char* version() const override { return "0.0.0-null"; }

    void onAttach(RuntimeContext&) override {
        LOG_INFO("Audio", "audio provider 'none' — no device opened");
    }
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}
};
