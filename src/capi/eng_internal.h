#pragma once
// ── eng_internal — first-party and test access under the C API (WO-070) ──────
// NOT part of the C API and never installed. Tests use it to set the world up
// through engine internals, because the C world API (WO-073) does not exist
// yet. Delete each use once eng_world.h can do the same.
#include <eng/engine.h>

class EngineRuntime;

// The runtime under `engine`, starting it headless if it has not started.
// NULL (with engGetLastErrorMessage set) if it cannot start.
EngineRuntime* engInternalRuntime(EngEngine* engine);

// ── Between eng_capi.cpp and eng_world.cpp (WO-073) ──────────────────────────
#include <string>
// The engine's one world handle, or null; eng_world.cpp sets it.
EngWorld*& engInternalWorldSlot(EngEngine* engine);
// Called by engDestroyEngine before the runtime shuts down.
void engInternalDestroyWorld(EngWorld* world);
// Sets this thread's engGetLastErrorMessage.
void engInternalSetError(std::string message);
// The flecs id behind a component handle, for tests and benches; 0 if none.
uint64_t engInternalComponentId(EngWorld* world, EngComponent component);

// ── Modules (eng_module.h, WO-073 part 3) ────────────────────────────────────
// True while a module's callback runs: no world is created or destroyed then.
bool engInternalInModuleCall(EngEngine* engine);
// on_world_start for every module, in order. One failing stops the ones
// started, in reverse, and the error names it.
EngResult engInternalStartModules(EngEngine* engine, EngWorld* world);
// on_world_stop for every module started, in reverse.
void engInternalStopModules(EngEngine* engine, EngWorld* world);
