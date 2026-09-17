$input a_position, a_texcoord0
$output v_texcoord0
#include <bgfx_shader.sh>

// Fullscreen triangle for the output pass (colour pipeline stage A). Positions
// arrive already in clip space and the UVs already account for the backend's
// texture origin (Renderer::submitOutput builds them), so this is a pass-through:
// no view transform is set on output views and none is wanted.
void main() {
    gl_Position = vec4(a_position.xy, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
