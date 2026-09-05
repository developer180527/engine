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

// Host-only: point draw submission at the runtime's renderer.
//
// NOT nullptr when headless — that was true before IRenderer and is not now. A
// headless host binds a NullRenderer, which counts submissions and discards them
// at frame end, so a kit that draws runs unchanged on a server. nullptr means
// only "before bind or after unbind", i.e. boot and teardown, where submitMesh
// no-ops.
struct IRenderer;   // render/renderer_interface.h
void engineDrawSubmitBindRenderer(IRenderer* r);
