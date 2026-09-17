#pragma once
// ── display_output — what the screen at the other end can show ──────────────
//
// Colour pipeline stage C. Stage B ended at an sRGB-encoded [0,1] image, which
// is what an SDR display wants. An HDR display wants something else entirely:
// LINEAR values, unencoded, where 1.0 means SDR reference white and anything
// above it is headroom.
//
// This type is what the renderer is told about the screen, and the output pass
// branches on it. It is deliberately tiny and platform-free — the platform layer
// fills it in (runtime/platform/hdr_surface.h), core only defines the vocabulary.
#include <cstdint>

namespace display {

enum class OutputEncoding : uint8_t {
    // sRGB-encoded [0,1]. Every SDR path, and what the editor's panels get,
    // because ImGui composites them into an 8-bit swapchain.
    SdrSrgb        = 0,
    // Linear, unencoded, 1.0 = SDR reference white, values above it allowed:
    // Apple EDR (extended linear sRGB) and Windows scRGB are both this shape.
    // The difference between them is `unitScale` below, not the encoding.
    ExtendedLinear = 1,
    Count
};

struct DisplayOutput {
    OutputEncoding encoding = OutputEncoding::SdrSrgb;

    // Peak white in SDR-WHITE UNITS: 1 = SDR, 4 = four times SDR white. On EDR
    // this is the headroom the system reports, and it moves with the user's
    // brightness slider — so it is read per frame.
    float headroom = 1.0f;

    // The last multiply before the value leaves the shader, so that 1.0 means
    // SDR reference white on THIS surface:
    //   EDR    1.0 — the colour space is already SDR-white-relative
    //   scRGB  paperWhiteNits / 80, because scRGB defines 1.0 as 80 nits
    //          [vendor: Microsoft], which is dimmer than any paper white
    float unitScale = 1.0f;
};

}  // namespace display
