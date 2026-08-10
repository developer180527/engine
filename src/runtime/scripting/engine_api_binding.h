#pragma once
// Internal: binds the C API (include/engine/engine_api.h) to the runtime's
// canonical ScriptHost. Called by EngineRuntime at init/shutdown. The C
// functions no-op safely while unbound or while no world is attached.
class ScriptHost;
void engineApiBindHost(ScriptHost* host);

// Host-only: point the action C API at the runtime's InputManager
// (nullptr on shutdown). Mirrors engineApiBindHost.
namespace input { class InputManager; }
void engineInputBindManager(input::InputManager* m);

// Host-only: point the memory group's frameAlloc at the runtime's per-frame
// arena (nullptr on shutdown). Unbound, frameAlloc returns null — which the
// contract already calls a normal outcome, so a kit written against it keeps
// working in a host that has no arena.
namespace mem { class FrameArena; }
void engineMemBindFrameArena(mem::FrameArena* a);

// Host-only: point draw submission at the renderer (nullptr headless/shutdown).
// Unbound, submitMesh no-ops — a kit that draws runs unchanged on a server.
class Renderer;
void engineDrawSubmitBindRenderer(Renderer* r);
