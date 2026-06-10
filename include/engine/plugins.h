#pragma once
// Stock engine plugins — UI-free backends, safe for games without ImGui:
//
//   engine.plugins().add(std::make_shared<JoltPlugin>());       // physics
//   engine.plugins().add(std::make_shared<LuaScriptPlugin>());  // scripting
//   engine.plugins().add(std::make_shared<AudioPlugin>());      // audio
//   engine.attachPlugins();
//
// Heavier compile than <engine/plugin.h> (pulls Jolt + Lua + miniaudio
// headers) — include only in the TU that registers plugins.

#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"
