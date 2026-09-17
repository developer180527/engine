$input v_color0
#include <bgfx_shader.sh>
#include "colour.sh"

// Debug lines draw into the LINEAR scene target (colour pipeline stage A), and
// their vertex colours are authored as ordinary sRGB colours — engineDrawLine's
// red is the red a user picked. Decoded here so the output pass's encode returns
// exactly that colour; left encoded, every debug line would come out washed-out.
void main() {
    gl_FragColor = vec4(srgbToLinear(v_color0.rgb), v_color0.a);
}
