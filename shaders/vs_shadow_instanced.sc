// vs_shadow_instanced — vs_shadow with the model matrix from INSTANCE DATA.
//
// Depth-only, same as vs_shadow.sc; the only difference is where `model` comes
// from. The shadow pass draws every caster in the light's frustum, and those
// casters group by mesh+material exactly as the main pass's do — so the same run
// detection collapses them into one submit per group.
$input a_position, i_data0, i_data1, i_data2, i_data3
#include <bgfx_shader.sh>
void main() {
    mat4 model  = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    gl_Position = mul(u_viewProj, mul(model, vec4(a_position, 1.0)));
}
