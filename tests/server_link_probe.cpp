// ── server_link_probe — engine_runtime_server LINKS without a graphics API ───
//
// G1c step B's exit criterion, and the only form of it that is worth anything:
// a binary that links. tests/headless_include_probe.cpp already proves the
// HEADERS do not reach bgfx, but headers were never the hard part — the runtime
// still called Renderer methods whose definitions lived in bgfx translation
// units, so every binary contained the whole renderer whether it used one or not.
//
// This links engine_runtime_server, which is built from engine_runtime's own
// source list minus the TUs that name a graphics API, with bgfx and bimg absent
// from target_link_libraries and therefore absent from the include path too. If
// anything in the simulation, asset, animation, physics, scripting, jobs, nav or
// audio path acquires a graphics dependency, this stops linking and names the
// symbol.
//
// It BOOTS the engine rather than merely referencing it, because a static
// library only pulls the objects an executable actually needs — a probe that
// just links would prove very little about the TUs it never touched. Booting
// drags in the runtime, the services and the frame loop.
//
// bx IS PRESENT AND THAT IS DELIBERATE: it is bgfx's maths library, a separate
// question on its own schedule (docs/rhi/evidence-coupling.md §2.1).
#include <cstdio>
#include <memory>

#include "runtime/runtime.h"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("server_link_probe: booting a server-build runtime\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false;
    cfg.autoDetectProject = false;

    EngineRuntime rt;
    // makeDefaultPlatform() resolves to HeadlessPlatform in this build — there is
    // no windowing library linked to resolve to anything else.
    if (!rt.init(cfg)) {
        std::printf("  FAIL  init\n");
        return 1;
    }
    if (!rt.headless()) {
        std::printf("  FAIL  a server build reported a renderable platform\n");
        return 1;
    }

    // Tick it. A server's whole job is simulate-and-send, so the frame path is
    // what must work without a device, not merely construction.
    for (int i = 0; i < 8; ++i) {
        rt.tick(1.0f / 30.0f);   // sim + systems; returns false headless
        rt.frameEnd();
    }

    rt.shutdown();
    std::printf("server_link_probe: OK — booted, ticked and shut down with no "
                "graphics library linked\n");
    return 0;
}
