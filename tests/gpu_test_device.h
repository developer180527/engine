#pragma once
#include <cstdio>

#include <bgfx/bgfx.h>

#include "render/gpu.h"

// ── A headless device for tests, with the upload seam OPEN ──────────────────
//
// Use this instead of calling bgfx::init directly.
//
// Since G1 the asset path goes through gpu:: (render/gpu.h), which gates every
// create on a device flag so that a process with no GPU can still parse assets.
// Renderer::init sets that flag. A test that calls bgfx::init by hand does NOT,
// so gpu::copy returns null, every create returns an invalid handle, and the
// test fails somewhere far away with "GPU buffer creation failed" — which looks
// like a real defect and is not one.
//
// That footgun is why this is a shared helper rather than two lines copied into
// nine files: the pairing of "a device exists" with "the seam is open" has to be
// impossible to half-do.
//
// The Noop backend is deliberate. It gives out real handles and refuses to draw,
// which is exactly what the asset and registry tests need — and it is why
// bgfx::getStats()->numDraw is always 0 in tests, the reason rdiag::SubmitStats
// counts on our side of the API.
inline bool initTestDevice(uint16_t width = 16, uint16_t height = 16) {
    bgfx::renderFrame();                       // single-threaded, as the engine does
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width  = width;
    init.resolution.height = height;
    if (!bgfx::init(init)) {
        std::printf("FAIL bgfx init\n");
        return false;
    }
    gpu::setDeviceAvailable(true);
    bgfx::frame();
    return true;
}

// Close the seam BEFORE the device goes down, matching Renderer::shutdown: a
// resource destroyed after this point is a no-op instead of a call into a dead
// bgfx, which is what makes registry teardown order not matter.
inline void shutdownTestDevice() {
    gpu::setDeviceAvailable(false);
    bgfx::shutdown();
}
