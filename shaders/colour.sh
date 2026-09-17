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

vec3 srgbToLinear(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((max(c, vec3_splat(0.0)) + 0.055) / 1.055, vec3_splat(2.4));
    return mix(lo, hi, step(vec3_splat(0.04045), c));
}
