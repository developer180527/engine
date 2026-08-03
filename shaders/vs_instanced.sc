// vs_instanced — vs_triangle with the model matrix from INSTANCE DATA.
//
// Identical maths to vs_triangle.sc; the only difference is where `model` comes
// from. vs_triangle reads u_model[0], set per draw via bgfx::setTransform, which
// costs a draw call per object. Here the matrix arrives as four vertex
// attributes filled from an instance buffer, so N objects sharing one mesh and
// one material become ONE submit.
//
// Why that matters beyond CPU time: per-draw uniform data lands in bgfx's Metal
// uniform scratch buffer, which is a FIXED 8 MB with no bounds check
// (renderer_mtl.cpp:23). At ~1 KB per draw that buffer is exhausted at ~8192
// draws and bgfx writes past the end. Instance transforms go into a VERTEX
// buffer instead, so a 20 000-object run costs one draw's worth of uniforms
// rather than 20 000 — instancing buys crash headroom, not just frame time.
$input  a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

void main() {
    // Columns, in the order allocInstanceDataBuffer was filled (see
    // ForwardPipeline: a Mat4 copied straight in, so this is mtxFromCols).
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

    vec4 worldPos = mul(model, vec4(a_position, 1.0));
    v_worldPos    = worldPos.xyz;
    gl_Position   = mul(u_viewProj, worldPos);

    // Cofactor normal matrix — correct for non-uniform scale, no inverse().
    vec3 mc0 = model[0].xyz;
    vec3 mc1 = model[1].xyz;
    vec3 mc2 = model[2].xyz;
    vec3 nm0 = cross(mc1, mc2);
    vec3 nm1 = cross(mc2, mc0);
    vec3 nm2 = cross(mc0, mc1);

    v_worldNormal = normalize(nm0*a_normal.x + nm1*a_normal.y + nm2*a_normal.z);

    vec3 tan3 = a_tangent.xyz;
    vec3 wtan = normalize(nm0*tan3.x + nm1*tan3.y + nm2*tan3.z);
    v_worldTangent = vec4(wtan, a_tangent.w);

    v_texcoord0 = a_texcoord0;
}
