#pragma once
// ── output_transform — the whole output pass, as ONE CPU function ───────────
//
// Colour pipeline stage B. shaders/fs_output.sc runs four steps per pixel, and
// their ORDER is the design, not an implementation detail:
//
//   1. exposure   scene-referred LINEAR radiance, then clamped at 0
//   2. tone map   still linear, now display-referred [0,1]
//   3. encode     linear -> sRGB, clipped to [0,1]
//   4. grade      the 3D LUT — which therefore sees sRGB-ENCODED, DISPLAY-
//                 REFERRED values in [0,1], never linear light
//
// Each step already has a reference (display_transform.h, colour.h,
// cube_lut.h). What none of them pins is the ORDER, and the order is exactly
// what a later edit gets wrong: move the grade before the encode and every
// Resolve export renders crushed, with no error anywhere. So this function is
// the order, colour_test holds it to known values, and colour_test also reads
// fs_output.sc and fails if the shader's steps appear in any other sequence.
//
// ── THE CLAMP AT STEP 1 IS LOAD-BEARING ─────────────────────────────────────
// pbrNeutral is the Khronos reference, verbatim, and it is not defined for
// negative input: its toe offset `x - 6.25x²` goes MORE negative, is subtracted,
// and folds a negative colour into a bright one (pbrNeutral(-1) is ~0.99). A
// linear target can hold negatives — an out-of-gamut operation, a signed
// specular term — so the pipeline clamps before the tone map, in the shader and
// here, rather than editing the reference function to guard itself.
#include <algorithm>

#include "core/colour.h"
#include "core/cube_lut.h"
#include "core/display_output.h"
#include "core/display_transform.h"

namespace display {

inline Rgb outputPixel(Rgb hdr, float exposure, ToneMapper toneMapper,
                       const CubeLut* grade = nullptr, DisplayOutput out = {}) {
    auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };

    Rgb c = { std::max(hdr.r * exposure, 0.0f),
              std::max(hdr.g * exposure, 0.0f),
              std::max(hdr.b * exposure, 0.0f) };

    // ── HDR output: tone map to the display's peak, and STOP ────────────────
    // No sRGB encode — the surface is linear and wants values above 1. And no
    // grade: a .cube here is display-referred sRGB [0,1] by construction
    // (LutLibrary refuses anything else), so there is nothing correct to do
    // with it against an extended-range image. The output pass reports that
    // once rather than applying it to a domain it was not authored for.
    if (out.encoding == OutputEncoding::ExtendedLinear) {
        const float peak = out.headroom > 1.0f ? out.headroom : 1.0f;
        c = toneMapper == ToneMapper::PbrNeutral
            ? pbrNeutralPeak(c, peak)
            : Rgb{ std::min(c.r, peak), std::min(c.g, peak), std::min(c.b, peak) };
        return { c.r * out.unitScale, c.g * out.unitScale, c.b * out.unitScale };
    }

    if (toneMapper == ToneMapper::PbrNeutral) c = pbrNeutral(c);

    float e[3] = { clamp01(colour::linearToSrgb(c.r)),
                   clamp01(colour::linearToSrgb(c.g)),
                   clamp01(colour::linearToSrgb(c.b)) };

    if (grade && grade->size >= 2) {
        applyCubeLut(*grade, e);
        for (float& v : e) v = clamp01(v);
    }
    return { e[0], e[1], e[2] };
}

}  // namespace display
