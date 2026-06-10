#pragma once
// Engine plugin interface + registry.
//
//   engine.plugins().add(std::make_shared<MyPlugin>());
//   engine.attachPlugins();
//
// Note: the stock plugins (JoltPlugin, LuaScriptPlugin, AudioPlugin in
// src/plugins/) currently include ImGui for their editor panels, so they are
// not re-exported here yet — include them directly and link imgui until the
// backend/UI split lands.

#include "runtime/plugin.h"
#include "runtime/plugin_registry.h"
#include "runtime/runtime_context.h"
