$input v_normal, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0);
uniform vec4 u_params;        // x = useTexture, yzw = baseColorFactor.rgb
uniform vec4 u_colorFactor;   // rgba base color multiplier

void main() {
    vec3 n = normalize(v_normal);

    if (u_params.x > 0.5) {
        vec4 tex = texture2D(s_baseColor, v_texcoord0);
        gl_FragColor = tex * u_colorFactor;
    } else {
        vec3 color = n * 0.5 + 0.5;
        gl_FragColor = vec4(color, 1.0);
    }
}
