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
