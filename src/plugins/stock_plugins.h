#pragma once
// ── stock_plugins — construct engine service providers from project data ────
// The one place provider NAMES map to plugin TYPES. Hosts (editor,
// engine_host, engine_player) call addStockPlugins() instead of hardcoding
// make_shared<JoltPlugin>() — so a user swaps physics/audio/scripting by
// editing project.json's "providers" block, not host code:
//
//   "providers": { "physics": "jolt", "scripting": "lua", "audio": "miniaudio" }
//
// Adding a NEW provider (PhysX, FMOD, …): write the plugin (an
// IEnginePlugin that also implements the service interface — JoltPlugin is
// the template: `class PhysXPlugin : IEnginePlugin, IPhysicsService`), add
// its case here, name it in project.json. Gameplay code is untouched — it
// only ever sees IPhysicsService / the enginePhysics* C API.
//
// Unknown names fall back to the default LOUDLY — a typo'd provider must
// not silently strip physics from a game.
#include "runtime/runtime.h"
#include "core/logger.h"

#include "plugins/jolt_plugin.h"
#include "plugins/null_physics_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/null_script_plugin.h"
#include "plugins/audio_plugin.h"

inline void addStockPlugins(EngineRuntime& engine) {
    const auto& p = engine.project().providers;

    // Physics — broadcast order puts it after scripting's intent writes,
    // but registration order here is attach order, matching the old
    // hardcoded sequence: physics, scripting, audio.
    if (p.physics == "jolt") {
        engine.plugins().add(std::make_shared<JoltPlugin>());
    } else if (p.physics == "none") {
        engine.plugins().add(std::make_shared<NullPhysicsPlugin>());
    } else {
        LOG_ERROR("Providers", "unknown physics provider '%s' — using 'jolt' "
                  "(known: jolt, none)", p.physics.c_str());
        engine.plugins().add(std::make_shared<JoltPlugin>());
    }

    if (p.scripting == "lua") {
        engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    } else if (p.scripting == "none") {
        engine.plugins().add(std::make_shared<NullScriptPlugin>());
    } else {
        LOG_ERROR("Providers", "unknown scripting provider '%s' — using 'lua' "
                  "(known: lua, none)", p.scripting.c_str());
        engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    }

    if (p.audio == "miniaudio") {
        engine.plugins().add(std::make_shared<AudioPlugin>());
    } else if (p.audio == "none") {
        // No null-audio plugin needed: ScriptHost warns once when audio is
        // reached for with no service bound.
    } else {
        LOG_ERROR("Providers", "unknown audio provider '%s' — using "
                  "'miniaudio' (known: miniaudio, none)", p.audio.c_str());
        engine.plugins().add(std::make_shared<AudioPlugin>());
    }
}
