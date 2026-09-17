#pragma once
// ── display_transform — exposure and tone mapping, the CPU reference ────────
//
// Colour pipeline stage B (docs/plans/colour-pipeline.md §4). The scene renders
// LINEAR light with no upper bound; a display shows [0,1]. What happens between
// is two steps, in this order, and this header is the reference for both:
//
//   exposure    scale scene radiance so the interesting range lands near 1
//   tone map    compress what is still above ~0.76 instead of clipping it
//
// then colour.h's encode and, optionally, a grading LUT (core/cube_lut.h).
// shaders/colour.sh runs the same maths per pixel; tests/colour_test.cpp checks
// its constants against the ones here, exactly as stage A does for the sRGB
// curve.
//
// ── EVIDENCE, per the plan's own ladder ─────────────────────────────────────
//   ev100 / exposureFromEv100   [vendor] Filament, filament/src/Exposure.cpp —
//                               the Lagarde/de Rousiers (Frostbite) convention.
//                               1.2 = 78 / (q·S), q = 0.65 lens attenuation,
//                               S = 100.
//   pbrNeutral                  [vendor] KhronosGroup/ToneMapping,
//                               PBR_Neutral/pbrNeutral.glsl, copied constant for
//                               constant.
//   pbrNeutralInverse           derived here from the forward function, and
//                               held to it by colour_test's round trip rather
//                               than by any claim in this comment.
//
// ── AgX IS NOT HERE, AND WHY ────────────────────────────────────────────────
// §4 recommends AgX as the shipped filmic alternative. The references disagree:
// Filament's AgX takes Rec.2020 input with an 8-term contrast fit, three.js and
// the "minimal AgX" it credits take Rec.709 through a conversion with a 7-term
// fit. Two constant sets for one name, and a subtly wrong tone curve is an error
// no test here can see. So ToneMapper has room for it and nothing more, until
// the constants are copied from one named source file.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace display {

// Both enums are stored in scenes: append only, never renumber.
enum class ToneMapper : uint8_t {
    None       = 0,   // stage A's behaviour: encode, then clip at 1
    PbrNeutral = 1,   // Khronos PBR Neutral — the engine default
    Count
};

enum class ExposureMode : uint8_t {
    Manual   = 0,     // exposureEV is a plain gain in stops: 2^EV
    Physical = 1,     // aperture / shutter / ISO, then exposureEV on top
    Count
};

// ── Exposure ─────────────────────────────────────────────────────────────────
// EV100 from camera settings. Higher EV100 = more light needed = DARKER image.
inline float ev100(float aperture, float shutterSeconds, float iso) {
    return std::log2((aperture * aperture) / shutterSeconds * 100.0f / iso);
}

// The scale applied to scene radiance for an EV100. PHYSICAL UNITS REQUIRED:
// at sunny-16 settings (EV100 ~ 15) this is ~2.5e-5, which is right for a sun
// of ~100 000 lux and makes this engine's current unitless lights (a sun of
// intensity 3) render black. That is why Manual is the default mode.
inline float exposureFromEv100(float ev) {
    return 1.0f / (1.2f * std::pow(2.0f, ev));
}

// Manual: +1 stop doubles the image, -1 halves it, 0 changes nothing.
inline float exposureGain(float stops) { return std::pow(2.0f, stops); }

// ── Khronos PBR Neutral ──────────────────────────────────────────────────────
// Preserves base colour, hue and saturation below the compression knee, which
// is why it is the default: an artist grades FROM it, and a CAD or product view
// shows authored colours as authored.
inline constexpr float kPbrToeEnd           = 0.08f;
inline constexpr float kPbrToeScale         = 6.25f;
inline constexpr float kPbrOffset           = 0.04f;
inline constexpr float kPbrStartCompression = 0.8f - 0.04f;   // 0.76
inline constexpr float kPbrDesaturation     = 0.15f;

struct Rgb { float r, g, b; };

inline Rgb pbrNeutral(Rgb c) {
    const float x = std::min(c.r, std::min(c.g, c.b));
    const float offset = x < kPbrToeEnd ? x - kPbrToeScale * x * x : kPbrOffset;
    c.r -= offset; c.g -= offset; c.b -= offset;

    const float peak = std::max(c.r, std::max(c.g, c.b));
    if (peak < kPbrStartCompression) return c;

    const float d = 1.0f - kPbrStartCompression;
    const float newPeak = 1.0f - d * d / (peak + d - kPbrStartCompression);
    const float s = newPeak / peak;
    c.r *= s; c.g *= s; c.b *= s;

    // mix(c, vec3(newPeak), g)
    const float g = 1.0f - 1.0f / (kPbrDesaturation * (peak - newPeak) + 1.0f);
    return { c.r * (1.0f - g) + newPeak * g,
             c.g * (1.0f - g) + newPeak * g,
             c.b * (1.0f - g) + newPeak * g };
}

// The inverse, for inputs >= 0 (the forward function folds negative inputs onto
// positive ones in its toe, so it has no inverse there). Worked backwards:
//   * after the desaturation mix max(y) is still newPeak, so newPeak = max(y);
//   * newPeak = 1 - d²/(peak + d - s) solves to peak = d²/(1 - newPeak) - d + s;
//   * the toe offset leaves min = 6.25·x² below x = 0.08 (min < 0.04), and
//     x - 0.04 above it, so x is recoverable from min alone.
// Useful to any pass that must undo the display transform (UI composited into
// the scene, colour pickers); correct only as far as colour_test's round trip
// says it is.
inline Rgb pbrNeutralInverse(Rgb y) {
    constexpr float s = kPbrStartCompression;
    const float d = 1.0f - s;
    Rgb c0 = y;
    const float n = std::max(y.r, std::max(y.g, y.b));
    if (n >= s) {
        const float nn   = std::min(n, 1.0f - 1e-6f);   // newPeak -> 1 as peak -> inf
        const float peak = d * d / (1.0f - nn) - d + s;
        const float g    = 1.0f - 1.0f / (kPbrDesaturation * (peak - nn) + 1.0f);
        const float k    = peak / nn;
        auto undo = [&](float v) { return (v - nn * g) / (1.0f - g) * k; };
        c0 = { undo(y.r), undo(y.g), undo(y.b) };
    }
    const float m = std::max(0.0f, std::min(c0.r, std::min(c0.g, c0.b)));
    const float offset = m < kPbrOffset ? std::sqrt(m / kPbrToeScale) - m : kPbrOffset;
    return { c0.r + offset, c0.g + offset, c0.b + offset };
}

}  // namespace display
