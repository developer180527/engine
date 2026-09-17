#pragma once
// ── colour — the sRGB transfer function, as the reference everything obeys ───
//
// Colour pipeline stage A (docs/plans/colour-pipeline.md). The renderer shades
// in LINEAR light; textures and authored colours are stored sRGB-ENCODED; the
// display wants sRGB-encoded output. These two functions are the whole boundary
// between those worlds, and this header is the single place their constants
// live on the CPU.
//
// ── WHY IT IS A HEADER IN CORE ──────────────────────────────────────────────
// It is needed by the renderer (Kelvin light colours, clear colours), by tests,
// and by any tool — and it is pure arithmetic. core/ is the GPU-free, ECS-free
// layer, so this compiles in a unit test with nothing else linked.
//
// ── THE GPU COPY ────────────────────────────────────────────────────────────
// The same encode runs per pixel in shaders/colour.sh. Two copies of one
// constant set is how they drift, so tests/colour_test.cpp checks the shader
// file's constants against the ones here — a mismatch fails the build's unit
// lane rather than showing up as a subtly wrong image.
//
// ── THE CURVE ───────────────────────────────────────────────────────────────
// IEC 61966-2-1 piecewise sRGB: a linear toe near black, a 2.4 power segment
// above it. Not a plain 2.2 gamma — that approximation is off by up to ~0.7%
// at the toe, which is exactly where banding in dark scenes comes from.
#include <cmath>

namespace colour {

// Encoded-domain threshold below which sRGB is linear (decode side).
inline constexpr float kDecodeCutoff = 0.04045f;
// Linear-domain threshold below which sRGB is linear (encode side).
inline constexpr float kEncodeCutoff = 0.0031308f;
inline constexpr float kToeSlope     = 12.92f;
inline constexpr float kScale        = 1.055f;
inline constexpr float kOffset       = 0.055f;
inline constexpr float kExponent     = 2.4f;

// sRGB-encoded [0,1] -> linear [0,1]. What a GPU does when it samples a texture
// created with an sRGB format.
inline float srgbToLinear(float c) {
    return c <= kDecodeCutoff ? c / kToeSlope
                              : std::pow((c + kOffset) / kScale, kExponent);
}

// Linear -> sRGB-encoded. Values above 1 encode above 1; the output pass
// saturates after encoding, because stage A has no tone mapper to bring HDR
// highlights back into range first (that is stage B).
inline float linearToSrgb(float c) {
    return c <= kEncodeCutoff ? c * kToeSlope
                              : kScale * std::pow(c, 1.0f / kExponent) - kOffset;
}

}  // namespace colour
