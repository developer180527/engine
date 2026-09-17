$input v_texcoord0
#include <bgfx_shader.sh>
#include "colour.sh"

// ── The output pass: linear HDR scene -> the display image ───────────────────
// Colour pipeline stages A and B. The scene renders into a float target holding
// LINEAR light with no upper bound; the display, ImGui and the editor's panels
// expect sRGB-ENCODED values in [0,1]. This is the one place that boundary is
// crossed, in four steps whose ORDER is the design (colour-pipeline.md §2):
//
//   1. exposure    scene-referred: scale radiance            u_display.x
//   2. tone map    scene -> display-referred, still linear   u_display.y
//   3. encode      linear -> sRGB, then clip to [0,1]
//   4. grade       an optional 3D LUT, which sees encoded    u_display.zw
//                  [0,1] — the domain a Resolve or OCIO
//                  "Rec.709 / sRGB" .cube export expects
//
// The CPU references are core/display_transform.h, core/colour.h and
// core/cube_lut.h, and colour_test holds colour.sh's constants to them.
//
// ENCODE IN THE SHADER, not with an sRGB render target: the editor shows this
// image through ImGui::Image, and sampling an sRGB-format texture decodes it back
// to linear on read — the panel would come out dark.
SAMPLER2D(s_hdr, 0);
SAMPLER3D(s_lut, 1);

uniform vec4 u_display;    // x exposure, y tone mapper (0 None, 1 PBR Neutral),
                           // z grade enabled, w LUT size
uniform vec4 u_lutMin;     // xyz: the LUT's DOMAIN_MIN
uniform vec4 u_lutScale;   // xyz: 1 / (DOMAIN_MAX - DOMAIN_MIN)

void main() {
    vec4 hdr = texture2D(s_hdr, v_texcoord0);
    vec3 c = max(hdr.rgb * u_display.x, vec3_splat(0.0));

    // A uniform branch, not a variant per tone mapper: two cases do not earn a
    // shader permutation, and the output pass draws one triangle per view.
    if (u_display.y > 0.5) c = pbrNeutral(c);

    c = clamp(linearToSrgb(c), 0.0, 1.0);

    if (u_display.z > 0.5) {
        // Domain, then texel CENTRES: a LUT of N entries spans [0.5/N, 1-0.5/N]
        // in texture space, and sampling [0,1] directly would stretch the
        // outermost cells by half a texel — a grade that is subtly wrong at
        // black and white. Mirrors applyCubeLut.
        float n = u_display.w;
        vec3 t = clamp((c - u_lutMin.xyz) * u_lutScale.xyz, 0.0, 1.0);
        c = clamp(texture3D(s_lut, t * ((n - 1.0) / n) + vec3_splat(0.5 / n)).rgb,
                  0.0, 1.0);
    }

    gl_FragColor = vec4(c, hdr.a);
}
