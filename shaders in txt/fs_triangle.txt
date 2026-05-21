$input v_normal, v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor, 0);
uniform vec4 u_params;       // x = hasTexture
uniform vec4 u_colorFactor;  // rgba multiplier
uniform vec4 u_lightDir;     // xyz = direction TOWARD light (world space, normalized)
uniform vec4 u_lightParams;  // x = ambient intensity (0.0-1.0)

void main() {
    vec3 n = normalize(v_normal);

    // Lambertian diffuse + ambient
    float NdotL   = max(dot(n, normalize(u_lightDir.xyz)), 0.0);
    float ambient = u_lightParams.x;
    float light   = ambient + (1.0 - ambient) * NdotL;

    vec4 baseColor;
    if (u_params.x > 0.5) {
        baseColor = texture2D(s_baseColor, v_texcoord0) * u_colorFactor;
    } else {
        // Normal-debug mode — still lit so geometry reads correctly
        baseColor = vec4(n * 0.5 + 0.5, 1.0);
    }

    gl_FragColor = vec4(baseColor.rgb * light, baseColor.a);
}
