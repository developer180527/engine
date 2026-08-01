$input v_worldPos, v_worldNormal, v_worldTangent, v_texcoord0
#include <bgfx_shader.sh>

#define MAX_LIGHTS 16

SAMPLER2D(s_baseColor,  0);
SAMPLER2D(s_normalMap,  1);
SAMPLER2D(s_shadowMap,  2);

// MATERIAL-OWNED. A cooked material writes this whole vec4, so nothing
// engine-driven may live here — a material's uniform block covers the entire
// register and would zero anything it does not know about.
uniform vec4 u_params;       // y=roughness z=metallic  (x,w reserved)
// ENGINE-DRIVEN. Which optional textures the renderer actually bound. Derived
// from what is resident, never authored, so it is kept out of u_params.
uniform vec4 u_texFlags;     // x=hasBaseColor y=hasNormalMap
uniform vec4 u_colorFactor;
uniform vec4 u_lightParams;  // x=ambient  y=lightCount
uniform vec4 u_camPos;
// Per light (4 vec4): [0] xyz=position, w=type(0 dir,1 point,2 spot)
//                     [1] rgb=color,    w=intensity
//                     [2] xyz=direction(toward light, directional), w=range
//                     [3] x=spotInnerCos, y=spotOuterCos
uniform vec4 u_lights[MAX_LIGHTS * 4];
uniform mat4 u_shadowMtx;
uniform vec4 u_shadowParams; // x=enabled y=bias z=texel w=shadowLightIndex

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
float sampleShadow(vec3 worldPos, float NdotL) {
    vec4 sc = mul(u_shadowMtx, vec4(worldPos, 1.0));
    vec3 uv = sc.xyz / sc.w;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || uv.z > 1.0) return 1.0;
    float bias  = max(u_shadowParams.y * (1.0 - NdotL), u_shadowParams.y * 0.25);
    float texel = u_shadowParams.z;
    float vis = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float d = texture2D(s_shadowMap, uv.xy + vec2(x, y) * texel).x;
            vis += (uv.z - bias <= d) ? 1.0 : 0.0;
        }
    return vis / 9.0;
}

void main() {
    float roughness = clamp(u_params.y, 0.045, 1.0);
    float metallic  = u_params.z;

    vec4 albedo = (u_texFlags.x > 0.5)
        ? texture2D(s_baseColor, v_texcoord0) * u_colorFactor
        : u_colorFactor;
    if (albedo.a < 0.01) discard;
    vec3 baseColor = albedo.rgb;

    vec3 N_geo = normalize(v_worldNormal);
    vec3 N;
    if (u_texFlags.y > 0.5) {
        vec3 N_ts = texture2D(s_normalMap, v_texcoord0).xyz * 2.0 - vec3_splat(1.0);
        vec3 T  = normalize(v_worldTangent.xyz);
        T = normalize(T - dot(T, N_geo) * N_geo);
        vec3 B  = cross(N_geo, T) * v_worldTangent.w;
        N = normalize(T * N_ts.x + B * N_ts.y + N_geo * N_ts.z);
    } else {
        N = N_geo;
    }

    vec3  V     = normalize(u_camPos.xyz - v_worldPos);
    float NdotV = max(dot(N, V), 0.001);
    vec3  F0    = mix(vec3_splat(0.04), baseColor, metallic);

    int   count  = int(u_lightParams.y);
    vec3  direct = vec3_splat(0.0);

    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (i >= count) break;

        vec4 P  = u_lights[i * 4 + 0];   // xyz pos,   w type
        vec4 Co = u_lights[i * 4 + 1];   // rgb color, w intensity
        vec4 Dr = u_lights[i * 4 + 2];   // xyz dir,   w range
        vec4 Sd = u_lights[i * 4 + 3];   // x innerCos, y outerCos

        vec3  L;
        float atten = 1.0;
        if (P.w < 0.5) {
            L = normalize(Dr.xyz);                 // directional
        } else {
            vec3  toL  = P.xyz - v_worldPos;       // point / spot
            float dist = length(toL);
            L = toL / max(dist, 0.0001);
            float dr  = dist / max(Dr.w, 0.0001);
            float win = clamp(1.0 - dr * dr * dr * dr, 0.0, 1.0);
            atten = (win * win) / (dist * dist + 1.0);
            if (P.w > 1.5) {
                float cone = clamp((dot(L, normalize(Dr.xyz)) - Sd.y) / max(Sd.x - Sd.y, 0.0001), 0.0, 1.0);
                atten *= cone * cone;
            }
        }

        vec3  H     = normalize(L + V);
        float NdotL = max(dot(N, L), 0.0);
        float vis = 1.0;
        if (u_shadowParams.x > 0.5 && i == int(u_shadowParams.w))
            vis = sampleShadow(v_worldPos, NdotL);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        float D = distributionGGX(NdotH, roughness);
        float G = geometrySchlick(NdotV, roughness) * geometrySchlick(NdotL, roughness);
        vec3  F = fresnelSchlick(HdotV, F0);

        vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 0.001);
        vec3 kD       = (vec3_splat(1.0) - F) * (1.0 - metallic);
        vec3 diffuse  = kD * baseColor / 3.14159265;

        direct += (diffuse + specular) * Co.rgb * Co.w * atten * NdotL * vis;
    }

    vec3 ambient = baseColor * u_lightParams.x;
    gl_FragColor = vec4(ambient + direct, albedo.a);
}
