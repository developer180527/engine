#pragma once
// ── clear_colour — a view's clear colour, into a LINEAR target ──────────────
//
// Colour pipeline stage A. Scene views render linear light, and a clear colour
// is authored as an ordinary sRGB colour (the editor's 0.102 grey, a camera's
// background), so it is decoded before it reaches the target.
//
// ── WHY THE PALETTE, AND WHY NOT ALWAYS ─────────────────────────────────────
// bgfx's packed clear is 8 bits per channel, and a DARK colour decoded to linear
// loses most of its precision there: 0.102 sRGB -> 0.0103 linear -> 3/255 ->
// displays as 0.110, a visible shift on exactly the greys editors use. bgfx's
// float palette keeps the precision.
//
// But the palette is ONE per frame, shared by every view — 16 slots, and a slot
// holds the last colour written to it. The first version keyed the slot as
// `viewId % 16`, so view 17 and view 1 shared a slot and whichever set its
// colour last cleared BOTH. Nothing hit it (only views 1 and 4 clear today, and
// RenderContext::allocView had no callers), which is exactly why it is decided
// here, in a function tests/colour_test.cpp can call, rather than inline in the
// pass: the first pipeline to allocate a sixteenth view would otherwise have
// found it as a wrong background colour with nothing in the log.
//
// So: views 0..15 own the slot equal to their id — unique, never shared — and
// any higher view clears with the packed colour instead, rounded rather than
// truncated. It loses precision in the darks; it cannot take another view's
// colour.
#include <cstdint>

#include "core/colour.h"
#include "render/gpu.h"

inline constexpr uint16_t kClearPaletteSlots = 16;   // BGFX_CONFIG_MAX_COLOR_PALETTE

struct ViewClearColour {
    bool     usePalette = false;       // true: palette[slot] = linear, clear by slot
    uint8_t  slot       = 0;
    float    linear[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };
    uint32_t packedRgba = 0x000000ff;  // the clear when !usePalette
};

// `r, g, b` are sRGB-encoded [0,1]; `a` is coverage and is never encoded.
inline ViewClearColour viewClearColour(gpu::ViewId view,
                                       float r, float g, float b, float a) {
    ViewClearColour c;
    c.linear[0] = colour::srgbToLinear(r);
    c.linear[1] = colour::srgbToLinear(g);
    c.linear[2] = colour::srgbToLinear(b);
    c.linear[3] = a;

    if (view < kClearPaletteSlots) {
        c.usePalette = true;
        c.slot = (uint8_t)view;
    }

    auto q = [](float v) -> uint32_t {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return (uint32_t)(v * 255.0f + 0.5f);
    };
    c.packedRgba = q(c.linear[0]) << 24 | q(c.linear[1]) << 16
                 | q(c.linear[2]) << 8  | q(c.linear[3]);
    return c;
}
