#pragma once
// ── hdr_surface — turn the window's surface into an extended-range one ───────
//
// Colour pipeline stage C. A WINDOW-SYSTEM concern, not a windowing-LIBRARY one,
// so it is selected by OS rather than by backend — exactly like title_bar.h.
// GLFW and SDL3 both hand back an NSWindow* on macOS, so one implementation
// serves both.
//
// ── macOS: configure the layer BEFORE bgfx initialises ──────────────────────
// bgfx's Metal backend never enables EDR itself. Given an NSWindow it looks at
// the content view's layer and, if that layer is already a CAMetalLayer, USES IT
// AS IS (renderer_mtl.cpp, SwapChainMtl::init). So the engine installs its own
// CAMetalLayer with EDR switched on and the extended-linear colour space, and
// bgfx adopts it — no change to the native handle, no patch to bgfx.
//
// bgfx sets the layer's pixel format, drawable size, vsync and max frame latency
// on init and on every resize, and touches NOTHING else — not the colour space,
// not wantsExtendedDynamicRangeContent. That is why the settings survive; it is
// read from its source, and `describeHdrSurface` exists so a run can SHOW that
// they did rather than assuming it.
//
// EXTENDED LINEAR sRGB, not Display P3: the engine shades in linear Rec.709/sRGB
// primaries, and handing those values to a P3 colour space would silently
// over-saturate every colour on screen. (The plan's §5 C2 said P3; that would
// have been a gamut error.)
#include <string>

namespace platwin {

// Configure `nativeWindow`'s surface for extended-range output. True only if the
// surface will now accept linear values above 1.0. False — unchanged, still SDR —
// on every platform without an implementation, and on macOS if the window has no
// content view yet. Must be called on the main thread, before renderer init.
bool enableExtendedDynamicRange(void* nativeWindow);

// The display's peak white in SDR-WHITE UNITS (1.0 = SDR, 4.0 = four times SDR
// white). CHANGES WITH SCREEN BRIGHTNESS, so it is read per frame, never stored.
// 1.0 where there is no implementation or no EDR.
float extendedDynamicRangeHeadroom(void* nativeWindow);

// One line naming what the surface actually is right now — layer class, EDR
// flag, colour space, pixel format. For the log, so "HDR is on" is an
// observation instead of a claim.
std::string describeHdrSurface(void* nativeWindow);

}  // namespace platwin
