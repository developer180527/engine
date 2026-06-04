$input a_position
#include <bgfx_shader.sh>
void main() {
    mat4 model  = u_model[0];
    gl_Position = mul(u_viewProj, mul(model, vec4(a_position, 1.0)));
}
