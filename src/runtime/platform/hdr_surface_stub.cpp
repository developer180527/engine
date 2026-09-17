// ── hdr_surface_stub — no extended-range output on this platform ─────────────
// Windows and Linux link this. The renderer side of HDR output is platform-
// neutral and built (an RGBA16F backbuffer plus the extended-linear branch in
// the output pass); what is missing here is the WINDOW-SYSTEM half.
//
// ── WINDOWS, AND WHY IT IS A STUB RATHER THAN A GUESS ───────────────────────
// bgfx already picks the swapchain colour space FROM THE BACKBUFFER FORMAT
// (dxgi.cpp, Dxgi::updateHdr10): R16G16B16A16_FLOAT selects scRGB
// (RGB_FULL_G10_NONE_P709). So `Renderer::requestHdrBackbuffer` is most of the
// Windows path already. What is NOT here:
//
//   * Deciding whether the display is in HDR mode at all. bgfx reads
//     DXGI_OUTPUT_DESC1 for its own trace but exposes none of it, so the engine
//     would query IDXGIOutput6::GetDesc1 itself.
//   * scRGB's paper white. 1.0 is 80 nits [vendor: Microsoft], far dimmer than
//     any paper white, so DisplayOutput::unitScale must be paperWhite/80 —
//     from MaxLuminance, or from a calibration screen.
//
// Both are a few dozen lines, and neither can be written honestly here: this
// tree has no Windows toolchain to compile them and no HDR Windows display to
// verify them against. Returning false means a Windows build behaves exactly as
// it does today rather than claiming an untested HDR path.
#include "runtime/platform/hdr_surface.h"

namespace platwin {

bool  enableExtendedDynamicRange(void*)      { return false; }
float extendedDynamicRangeHeadroom(void*)    { return 1.0f; }
std::string describeHdrSurface(void*)        { return "not implemented on this platform"; }

}  // namespace platwin
