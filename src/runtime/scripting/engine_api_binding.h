#pragma once
// Internal: binds the C API (include/engine/engine_api.h) to the runtime's
// canonical ScriptHost. Called by EngineRuntime at init/shutdown. The C
// functions no-op safely while unbound or while no world is attached.
class ScriptHost;
void engineApiBindHost(ScriptHost* host);
