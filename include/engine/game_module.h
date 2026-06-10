#pragma once
// ── Game module contract — C++ hot reload ───────────────────────────────────
// A game compiles to a shared library exposing these C entry points; the
// engine_host dev-runner loads it, registers the plugin, and reloads the
// library whenever it changes on disk. All world state (entities, components,
// assets) lives in the HOST — it survives reloads. Module-internal state does
// not: keep state in components, keep logic in the module.
//
//   class MyGame final : public IEnginePlugin { ... };
//   ENGINE_GAME_MODULE(MyGame)
//
// Reload cycle (sim keeps running, world persists):
//   old: onSimulationStop -> onDetach -> destroy -> dlclose
//   new: dlopen -> create -> onAttach -> onSimulationStart(same world)
//
// Rules the module must follow:
//   - Deregister anything pointing into the module (flecs systems,
//     callbacks) in onDetach/onSimulationStop — dangling pointers after
//     dlclose are instant crashes.
//   - Component structs are shared with the host: changing a component's
//     layout requires restarting the host, not just reloading.
#include "runtime/plugin.h"

#define ENGINE_GAME_API_VERSION 1

extern "C" {
typedef IEnginePlugin* (*EngineCreateGameFn)();
typedef void           (*EngineDestroyGameFn)(IEnginePlugin*);
typedef int            (*EngineGameApiVersionFn)();
}

// Place once in the module's main translation unit.
// Creation/destruction both happen inside the module so allocators match.
#define ENGINE_GAME_MODULE(PluginType)                                        \
    extern "C" {                                                              \
    __attribute__((visibility("default")))                                    \
    IEnginePlugin* engineCreateGame() { return new PluginType(); }            \
    __attribute__((visibility("default")))                                    \
    void engineDestroyGame(IEnginePlugin* p) { delete p; }                    \
    __attribute__((visibility("default")))                                    \
    int engineGameApiVersion() { return ENGINE_GAME_API_VERSION; }            \
    }
