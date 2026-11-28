#pragma once
// ── eng_internal — first-party and test access under the C API (WO-070) ──────
// NOT part of the C API and never installed. Tests use it to set the world up
// through engine internals, because the C world API (WO-073) does not exist
// yet. Delete each use once eng_world.h can do the same.
#include <eng/engine.h>

class EngineRuntime;
namespace flecs { struct world; }

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
// Reads and checks a module's description; at names it in the message.
EngResult engInternalReadModule(const EngModuleDesc* in, const std::string& at, EngModuleDesc& out, std::string& why);
// True while a module's callback runs: no world is created or destroyed then.
bool engInternalInModuleCall(EngEngine* engine);
// on_world_start for every module, in order. One failing stops the ones
// started, in reverse, and the error names it.
EngResult engInternalStartModules(EngEngine* engine, EngWorld* world);
// on_world_stop for every module started, in reverse.
void engInternalStopModules(EngEngine* engine, EngWorld* world);

// ── The world a project's C kits share (WO-094) ──────────────────────────────
// The C world of a simulation world: made at the first hold, and gone when the
// program (engCreateWorld) and every C kit have let go.
EngWorld* engInternalAcquireKitWorld(EngineRuntime* rt, flecs::world& world);
void engInternalReleaseKitWorld(EngWorld* world);
// A system or a query iteration is running in it.
bool engInternalWorldBusy(EngWorld* world);
// What is created from now on is owner's; returns the owner before.
const void* engInternalSetOwner(EngWorld* world, const void* owner);
// Destroys owner's systems and queries (a C kit stopping, or reloading);
// returns how many.
uint32_t engInternalDropOwned(EngWorld* world, const void* owner);
// C modules from a project's manifest: installs the loader on rt's kit host.
void engInternalHostCModules(EngineRuntime* rt);
