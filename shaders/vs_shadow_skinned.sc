$input a_position, a_indices, a_weight
#include <bgfx_shader.sh>

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
    ivec4 idx = ivec4(a_indices * 255.0 + 0.5);
    mat4 skin = getBoneMatrix(idx.x) * a_weight.x
              + getBoneMatrix(idx.y) * a_weight.y
              + getBoneMatrix(idx.z) * a_weight.z
              + getBoneMatrix(idx.w) * a_weight.w;
    vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));
    mat4 model  = u_model[0];
    gl_Position = mul(u_viewProj, mul(model, skinnedPos));
}
