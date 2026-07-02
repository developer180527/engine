$input  a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight
$output v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

// Bone palette — 128 bones max (128 * 4 = 512 vec4 uniforms).
uniform vec4 u_boneMatrices[512];

mat4 getBoneMatrix(int idx) {
    int base = idx * 4;
    return mat4(
        u_boneMatrices[base + 0],
        u_boneMatrices[base + 1],
        u_boneMatrices[base + 2],
        u_boneMatrices[base + 3]
    );
}

void main() {
    // Decode bone indices (bgfx normalizes uint8 to [0,1] for Attrib::Indices)
    ivec4 idx = ivec4(a_indices * 255.0 + 0.5);

    // Linear blend skinning (LBS)
    mat4 skin = getBoneMatrix(idx.x) * a_weight.x
              + getBoneMatrix(idx.y) * a_weight.y
              + getBoneMatrix(idx.z) * a_weight.z
              + getBoneMatrix(idx.w) * a_weight.w;

    // ROW-VECTOR multiply (v * M): the palette is uploaded as a raw vec4[]
    // array, so it arrives in bx's row-major memory layout UNTOUCHED — unlike
    // u_model, which bgfx preps for the mul(M, v) convention. Diagnosed
    // empirically: mul(skin, v) rendered exploded meshes; transposed palettes
    // (== this multiply order) render correctly. Keep AnimatorSystem palettes
    // bx-native and flip the order here instead.
    vec4 skinnedPos    = mul(vec4(a_position, 1.0),    skin);
    vec3 skinnedNormal = mul(vec4(a_normal, 0.0),      skin).xyz;
    vec3 skinnedTan    = mul(vec4(a_tangent.xyz, 0.0), skin).xyz;

    mat4 model    = u_model[0];
    vec4 worldPos = mul(model, skinnedPos);
    v_worldPos    = worldPos.xyz;
    gl_Position   = mul(u_viewProj, worldPos);

    // Cofactor normal matrix — same technique as vs_triangle.sc
    vec3 mc0 = model[0].xyz;
    vec3 mc1 = model[1].xyz;
    vec3 mc2 = model[2].xyz;
    vec3 nm0 = cross(mc1, mc2);
    vec3 nm1 = cross(mc2, mc0);
    vec3 nm2 = cross(mc0, mc1);

    v_worldNormal  = normalize(nm0*skinnedNormal.x + nm1*skinnedNormal.y + nm2*skinnedNormal.z);
    vec3 wtan      = normalize(nm0*skinnedTan.x    + nm1*skinnedTan.y    + nm2*skinnedTan.z);
    v_worldTangent = vec4(wtan, a_tangent.w);

    v_texcoord0 = a_texcoord0;
}
