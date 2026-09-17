$input v_texcoord0
#include <bgfx_shader.sh>
#include "colour.sh"

// ── The output pass: linear HDR scene -> sRGB-encoded display image ──────────
// Colour pipeline stage A. The scene renders into a float target holding LINEAR
// light, which can exceed 1.0; the display, ImGui and the editor's scene panel
// all expect sRGB-ENCODED values in [0,1]. This is the one place that boundary
// is crossed.
//
// ENCODE IN THE SHADER, not with an sRGB render target: the editor shows this
// image through ImGui::Image, and sampling an sRGB-format texture decodes it back
// to linear on read — ImGui would then write linear values to a non-sRGB
// backbuffer and the panel would come out dark. Encoded bytes in a UNORM target
// look the same whether they reach the screen through a panel or the backbuffer.
//
// SATURATE AFTER ENCODING. Stage A has no exposure and no tone mapper, so a
// highlight brighter than 1.0 clips here exactly as it clipped in the old RGBA8
// target — nothing is lost that was not already lost. What is gained is that the
// clip now happens AFTER lighting instead of in the middle of it. Mapping those
// highlights back into range is stage B.
SAMPLER2D(s_hdr, 0);

void main() {
    vec4 hdr = texture2D(s_hdr, v_texcoord0);
    gl_FragColor = vec4(clamp(linearToSrgb(hdr.rgb), 0.0, 1.0), hdr.a);
}
