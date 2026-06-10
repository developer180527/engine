#pragma once
// Engine plugin interface + registry.
//
//   engine.plugins().add(std::make_shared<MyPlugin>());
//   engine.attachPlugins();
//
// Stock plugins (Jolt physics, Lua scripting, miniaudio) live in
// <engine/plugins.h> — separate include because their headers are heavy.

#include "runtime/plugin.h"
#include "runtime/plugin_registry.h"
#include "runtime/runtime_context.h"
