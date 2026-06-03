$input  a_position, a_normal, a_tangent, a_texcoord0
$output v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

void main() {
    mat4 model = u_model[0];

    vec4 worldPos = mul(model, vec4(a_position, 1.0));
    v_worldPos    = worldPos.xyz;
    gl_Position   = mul(u_viewProj, worldPos);

    // Cofactor normal matrix — correct for non-uniform scale,
    // no mat3 needed, no expensive inverse().
    vec3 mc0 = model[0].xyz;
    vec3 mc1 = model[1].xyz;
    vec3 mc2 = model[2].xyz;
    vec3 nm0 = cross(mc1, mc2);
    vec3 nm1 = cross(mc2, mc0);
    vec3 nm2 = cross(mc0, mc1);

    v_worldNormal = normalize(nm0*a_normal.x   + nm1*a_normal.y   + nm2*a_normal.z);

    // Same cofactor transform for tangent; preserve handedness in .w
    vec3 tan3 = a_tangent.xyz;
    vec3 wtan = normalize(nm0*tan3.x + nm1*tan3.y + nm2*tan3.z);
    v_worldTangent = vec4(wtan, a_tangent.w);

    v_texcoord0 = a_texcoord0;
}
