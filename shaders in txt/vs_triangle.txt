$input  a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0

#include <bgfx_shader.sh>

void main() {
    // When instancing: i_data0-3 contain the per-instance model matrix rows.
    // When not instancing: u_model[0] is used (bgfx sets it from setTransform).
    // bgfx handles this transparently — the same shader works for both paths.
    mat4 model = u_model[0];

    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_normal    = mul(model, vec4(a_normal, 0.0)).xyz;
    v_texcoord0 = a_texcoord0;
}
