// ── colour.sh — the sRGB transfer function, GPU side ─────────────────────────
// The per-pixel twin of src/core/colour.h. Its constants MUST match that file;
// tests/colour_test.cpp reads this file and fails if they do not, because a
// drifted constant here renders a subtly wrong image that no other test sees.
//
// Written branch-free with step(), so it is one expression per channel and is
// legal in every profile this build compiles (including GLSL 120 / ES 100).

vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3_splat(0.0)), vec3_splat(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3_splat(0.0031308), c));
}

// Khronos PBR Neutral (stage B). Constants mirror core/display_transform.h,
// which cites the reference file; colour_test checks them. 0.76 is the source's
// `0.8 - 0.04` written out, because the literal check compares numbers.
vec3 pbrNeutral(vec3 color) {
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= vec3_splat(offset);
    float peak = max(color.r, max(color.g, color.b));
    if (peak < 0.76) return color;
    float d = 1.0 - 0.76;
    float newPeak = 1.0 - d * d / (peak + d - 0.76);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (0.15 * (peak - newPeak) + 1.0);
    return mix(color, vec3_splat(newPeak), g);
}

// The same curve aimed at a display brighter than SDR white (stage C). Mirrors
// core/display_transform.h's pbrNeutralPeak: the knee and ceiling scale with the
// peak, the toe offset does NOT — it is a black level in SDR units.
vec3 pbrNeutralPeak(vec3 color, float peak) {
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= vec3_splat(offset);
    float pk = max(color.r, max(color.g, color.b));
    float s = 0.76 * peak;
    if (pk < s) return color;
    float d = peak - s;
    float newPeak = peak - d * d / (pk + d - s);
    color *= newPeak / pk;
    float g = 1.0 - 1.0 / (0.15 * (pk - newPeak) / peak + 1.0);
    return mix(color, vec3_splat(newPeak), g);
}

vec3 srgbToLinear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((max(c, vec3_splat(0.0)) + 0.055) / 1.055, vec3_splat(2.4));
    return mix(lo, hi, step(vec3_splat(0.04045), c));
}
