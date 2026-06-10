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
// Engine services from inside a module: use the C API (engine/engine_api.h).
// Those calls execute host-side, so input/audio/physics state is the real
// one. Header-inline singletons (Input::, InputSystem::get()) duplicate
// across the dylib boundary — don't use them in modules.
#include "runtime/plugin.h"

// ── Compatibility ────────────────────────────────────────────────────────────
// Two layers of checking at load time:
//
//   1. API version  — bumped manually whenever the IEnginePlugin interface
//      or this contract changes shape.
//   2. ABI fingerprint — compiler + language version + build config baked
//      into both sides AT COMPILE TIME. Catches the silent killers version
//      numbers can't: module built with a different compiler, different
//      C++ standard, or debug-vs-release against the host.
//
// The fingerprint deliberately does NOT include a per-build hash: the module
// is routinely newer than the host binary (that's the whole point of hot
// reload). The residual risk — host and module built from different SDK
// checkouts with identical toolchains but a changed IEnginePlugin vtable —
// is accepted and covered by rule 4 below: rebuild the host after pulling
// engine changes.
#define ENGINE_GAME_API_VERSION 1

#ifdef NDEBUG
    #define ENGINE_ABI_BUILD_MODE "release"
#else
    #define ENGINE_ABI_BUILD_MODE "debug"
#endif
#define ENGINE_ABI_STR2(x) #x
#define ENGINE_ABI_STR(x)  ENGINE_ABI_STR2(x)
#define ENGINE_ABI_FINGERPRINT \
    __VERSION__ "|c++" ENGINE_ABI_STR(__cplusplus) \
    "|api" ENGINE_ABI_STR(ENGINE_GAME_API_VERSION) "|" ENGINE_ABI_BUILD_MODE

// Why the host loaded the module — passed to the optional load hook so a
// module can distinguish a cold start from a live code swap.
typedef enum EngineModuleLoadReason {
    ENGINE_MODULE_LOAD_INITIAL   = 0,
    ENGINE_MODULE_LOAD_HOTRELOAD = 1,
} EngineModuleLoadReason;

extern "C" {
typedef IEnginePlugin* (*EngineCreateGameFn)();
typedef void           (*EngineDestroyGameFn)(IEnginePlugin*);
typedef int            (*EngineGameApiVersionFn)();
typedef const char*    (*EngineGameAbiFingerprintFn)();
// Optional export — implement `void engineGameLoadReason(int)` in the module
// if you want the load-reason callback; the host calls it before onAttach.
typedef void           (*EngineGameLoadReasonFn)(int);
}

// Place once in the module's main translation unit.
// Creation/destruction both happen inside the module so allocators match —
// the host must NEVER delete a plugin instance directly.
//
// Naming convention: `engineGame*` exports are reserved for game modules;
// future module kinds get their own prefix (engineEditor*, engineTool*, ...).
#define ENGINE_GAME_MODULE(PluginType)                                        \
    extern "C" {                                                              \
    __attribute__((visibility("default")))                                    \
    IEnginePlugin* engineCreateGame() { return new PluginType(); }            \
    __attribute__((visibility("default")))                                    \
    void engineDestroyGame(IEnginePlugin* p) { delete p; }                    \
    __attribute__((visibility("default")))                                    \
    int engineGameApiVersion() { return ENGINE_GAME_API_VERSION; }            \
    __attribute__((visibility("default")))                                    \
    const char* engineGameAbiFingerprint() { return ENGINE_ABI_FINGERPRINT; } \
    }

// ── Design notes & accepted risks ────────────────────────────────────────────
// This contract trades some safety for a fast dev loop. Known limits:
//
//  1. Mutable globals/statics in the module are reload hazards: background
//     threads, cached pointers, singletons and global registries silently
//     dangle after dlclose. Discipline is the mitigation — state belongs in
//     components, and anything pointing into the module must be released in
//     onDetach/onSimulationStop.
//  2. Component struct layout changes are NOT detectable here (the structs
//     are shared C++ headers). Changing one requires restarting the host.
//  3. The fingerprint can't see IEnginePlugin vtable changes between two
//     builds of the same toolchain — rebuild the host after engine updates.
//  4. This is a DEV-TIME contract, not a stable public ABI. Shipped games
//     link engine::runtime statically and never dlopen anything.
