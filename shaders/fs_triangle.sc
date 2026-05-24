$input v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

SAMPLER2D(s_baseColor,  0);
SAMPLER2D(s_normalMap,  1);

uniform vec4 u_params;       // x=hasBaseColor  y=roughness  z=metallic  w=hasNormalMap
uniform vec4 u_colorFactor;
uniform vec4 u_lightDir;     // xyz = toward light
uniform vec4 u_lightColor;   // xyz = RGB  w = intensity
uniform vec4 u_lightParams;  // x = ambient
uniform vec4 u_camPos;

float distributionGGX(float NdotH, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 0.0001);
}

float geometrySchlick(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k + 0.0001);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    float f = pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
    return F0 + (vec3_splat(1.0) - F0) * f;
}

void main() {
    float roughness = u_params.y;
    float metallic  = u_params.z;

    // Base color
    vec4 albedo = (u_params.x > 0.5)
        ? texture2D(s_baseColor, v_texcoord0) * u_colorFactor
        : u_colorFactor;
    if (albedo.a < 0.01) discard;
    vec3 baseColor = albedo.rgb;

    // Surface normal — geometry or normal-mapped
    vec3 N_geo = normalize(v_worldNormal);
    vec3 N;
    if (u_params.w > 0.5) {
        // Sample + decode normal map
        vec3 N_ts = texture2D(s_normalMap, v_texcoord0).xyz * 2.0 - vec3_splat(1.0);
        // Gram-Schmidt TBN (avoids mat3 type entirely)
        vec3 T  = normalize(v_worldTangent.xyz);
        T = normalize(T - dot(T, N_geo) * N_geo);      // re-orthogonalize
        vec3 B  = cross(N_geo, T) * v_worldTangent.w;  // handedness-correct bitangent
        // Rotate from tangent → world space: T*x + B*y + N*z
        N = normalize(T * N_ts.x + B * N_ts.y + N_geo * N_ts.z);
    } else {
        N = N_geo;
    }

    vec3 L = normalize(u_lightDir.xyz);
    vec3 V = normalize(u_camPos.xyz - v_worldPos);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3_splat(0.04), baseColor, metallic);

    float D = distributionGGX(NdotH, roughness);
    float G = geometrySchlick(NdotV, roughness) * geometrySchlick(NdotL, roughness);
    vec3  F = fresnelSchlick(HdotV, F0);

    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 0.001);
    vec3 kD       = (vec3_splat(1.0) - F) * (1.0 - metallic);
    vec3 diffuse  = kD * baseColor / 3.14159265;

    vec3 ambient = baseColor * u_lightParams.x;
    vec3 direct  = (diffuse + specular) * u_lightColor.rgb * u_lightColor.w * NdotL;

    gl_FragColor = vec4(ambient + direct, albedo.a);
}
