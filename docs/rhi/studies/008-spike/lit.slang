// Study 008 spike: the engine's lit shader (shaders/vs_triangle.sc + fs_triangle.sc)
// ported to Slang the way tier L would feed it. The lighting maths is unchanged;
// what changes is how data arrives:
//   - per-draw data from a structured buffer indexed by draw id (no per-draw uniforms, G4)
//   - textures from ONE bindless array, indexed by the material (axiom 2)
//   - lights from a structured buffer instead of u_lights[64]
static const uint MAX_LIGHTS = 16;

struct DrawData {
    float4x4 model;        // column-vector convention: world = mul(model, p)
    uint     material;     // index into materials[]
    uint3    pad;
};
struct Material {
    float4 colorFactor;
    float  roughness, metallic;
    DescriptorHandle<Texture2D> baseColorTex;   // the white texture when absent
    DescriptorHandle<Texture2D> normalTex;      // the flat normal when absent
};
struct Light {
    float4 position;       // xyz, w = type (0 dir, 1 point, 2 spot)
    float4 color;          // rgb, w = intensity
    float4 direction;      // xyz toward light, w = range
    float4 spot;           // x innerCos, y outerCos
};
struct Frame {
    float4x4 viewProj;
    float4x4 shadowMtx;
    float4   camPos;
    float4   lightParams;  // x ambient, y count
    float4   shadowParams; // x enabled, y bias, z texel, w light index
    DescriptorHandle<Texture2D> shadowTex;
};

[[vk::binding(1, 0)]] SamplerState            linearSampler;
[[vk::binding(2, 0)]] StructuredBuffer<DrawData> draws;
[[vk::binding(3, 0)]] StructuredBuffer<Material> materials;
[[vk::binding(4, 0)]] StructuredBuffer<Light>    lights;
[[vk::binding(5, 0)]] ConstantBuffer<Frame>      frame;

struct VSIn {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};
struct VSOut {
    float4 clip         : SV_Position;
    float3 worldPos     : POSITION1;
    float3 worldNormal  : NORMAL1;
    float4 worldTangent : TANGENT1;
    float2 uv           : TEXCOORD0;
    nointerpolation uint material : MATERIAL;
};

[shader("vertex")]
VSOut vsMain(VSIn i, uint drawId : SV_InstanceID) {
    DrawData d = draws[drawId];
    float4x4 m = d.model;
    VSOut o;
    float4 world = mul(m, float4(i.position, 1.0));
    o.worldPos = world.xyz;
    o.clip     = mul(frame.viewProj, world);
    // Cofactor normal matrix, as vs_triangle.sc: correct under non-uniform scale.
    float3 mc0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 mc1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 mc2 = float3(m[0][2], m[1][2], m[2][2]);
    float3 nm0 = cross(mc1, mc2), nm1 = cross(mc2, mc0), nm2 = cross(mc0, mc1);
    o.worldNormal  = normalize(nm0 * i.normal.x + nm1 * i.normal.y + nm2 * i.normal.z);
    float3 wt      = normalize(nm0 * i.tangent.x + nm1 * i.tangent.y + nm2 * i.tangent.z);
    o.worldTangent = float4(wt, i.tangent.w);
    o.uv       = i.uv;
    o.material = d.material;
    return o;
}

float distributionGGX(float NdotH, float r) { float a = r*r, a2 = a*a; float d = NdotH*NdotH*(a2-1.0)+1.0; return a2/(3.14159265*d*d+0.0001); }
float geometrySchlick(float NdotX, float r) { float k = (r+1.0)*(r+1.0)/8.0; return NdotX/(NdotX*(1.0-k)+k+0.0001); }
float3 fresnelSchlick(float c, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - c), 5.0); }

// Explicit LOD in a loop, as fs_triangle.sc (the X3570 rule): a shadow map has no mips.
float sampleShadow(float3 worldPos, float NdotL) {
    float4 sc = mul(frame.shadowMtx, float4(worldPos, 1.0));
    float3 uv = sc.xyz / sc.w;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1 || uv.z > 1) return 1.0;
    float bias = max(frame.shadowParams.y * (1.0 - NdotL), frame.shadowParams.y * 0.25);
    float texel = frame.shadowParams.z, vis = 0.0;
    Texture2D sm = frame.shadowTex;   // uniform: one shadow map per frame
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float d = sm.SampleLevel(linearSampler, uv.xy + float2(x, y) * texel, 0).x;
            vis += (uv.z - bias <= d) ? 1.0 : 0.0;
        }
    return vis / 9.0;
}

[shader("fragment")]
float4 fsMain(VSOut i) : SV_Target {
    Material mat = materials[i.material];
    float roughness = clamp(mat.roughness, 0.045, 1.0);
    float metallic  = mat.metallic;
    // The handle can differ across a draw's pixels when materials do, so say so:
    // without NonUniform, SPIR-V lets the GPU use one lane's texture for all of them.
    float4 baseTex   = nonuniform(mat.baseColorTex).Sample(linearSampler, i.uv);
    float3 normalTex = nonuniform(mat.normalTex).Sample(linearSampler, i.uv).xyz;
    float4 albedo = baseTex * mat.colorFactor;
    if (albedo.a < 0.01) discard;
    float3 baseColor = albedo.rgb;

    float3 Ng = normalize(i.worldNormal);
    float3 Nts = normalTex * 2.0 - 1.0;
    float3 T = normalize(i.worldTangent.xyz); T = normalize(T - dot(T, Ng) * Ng);
    float3 B = cross(Ng, T) * i.worldTangent.w;
    float3 N = normalize(T * Nts.x + B * Nts.y + Ng * Nts.z);

    float3 V = normalize(frame.camPos.xyz - i.worldPos);
    float NdotV = max(dot(N, V), 0.001);
    float3 F0 = lerp(float3(0.04), baseColor, metallic);
    uint count = min(uint(frame.lightParams.y), MAX_LIGHTS);
    float3 direct = 0;
    for (uint li = 0; li < count; ++li) {
        Light L0 = lights[li];
        float3 L; float atten = 1.0;
        if (L0.position.w < 0.5) {
            L = normalize(L0.direction.xyz);
        } else {
            float3 toL = L0.position.xyz - i.worldPos; float dist = length(toL);
            L = toL / max(dist, 0.0001);
            float dr = dist / max(L0.direction.w, 0.0001);
            float win = saturate(1.0 - dr*dr*dr*dr);
            atten = (win*win) / (dist*dist + 1.0);
            if (L0.position.w > 1.5) {
                float cone = saturate((dot(L, normalize(L0.direction.xyz)) - L0.spot.y) / max(L0.spot.x - L0.spot.y, 0.0001));
                atten *= cone * cone;
            }
        }
        float3 H = normalize(L + V);
        float NdotL = max(dot(N, L), 0.0);
        float vis = 1.0;
        if (frame.shadowParams.x > 0.5 && li == uint(frame.shadowParams.w)) vis = sampleShadow(i.worldPos, NdotL);
        float NdotH = max(dot(N, H), 0.0), HdotV = max(dot(H, V), 0.0);
        float D = distributionGGX(NdotH, roughness);
        float G = geometrySchlick(NdotV, roughness) * geometrySchlick(NdotL, roughness);
        float3 F = fresnelSchlick(HdotV, F0);
        float3 spec = (D * G) * F / max(4.0 * NdotV * NdotL, 0.001);
        float3 kD = (1.0 - F) * (1.0 - metallic);
        direct += (kD * baseColor / 3.14159265 + spec) * L0.color.rgb * L0.color.w * atten * NdotL * vis;
    }
    return float4(baseColor * frame.lightParams.x + direct, albedo.a);
}
