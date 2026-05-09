$input  a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));

    // Transform normal to world space.
    //
    // For a rotation-only matrix, mul(matrix, normal) is correct. With
    // non-uniform scale, the math is wrong (you'd need the inverse-transpose
    // of the upper-3x3). We don't do non-uniform scale on lit objects in
    // practice, so the simpler form is fine here. If we ever do, this is
    // the line to fix.
    v_normal = mul(u_model[0], vec4(a_normal, 0.0)).xyz;

    v_texcoord0 = a_texcoord0;
}
