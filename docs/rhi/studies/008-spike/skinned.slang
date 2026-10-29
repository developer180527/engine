// Study 008 spike: shaders/vs_skinned.sc, palette from a structured buffer
// (no 128-bone uniform ceiling: WO-053's limit is the uniform array, not the GPU).
struct DrawData { float4x4 model; uint material; uint paletteBase; uint2 pad; };
struct Frame { float4x4 viewProj; };
[[vk::binding(0, 0)]] StructuredBuffer<DrawData> draws;
[[vk::binding(1, 0)]] StructuredBuffer<float4x4> palette;   // every skinned mesh's bones, one buffer
[[vk::binding(2, 0)]] ConstantBuffer<Frame>      frame;

struct VSIn { float3 position : POSITION; float3 normal : NORMAL; float4 tangent : TANGENT;
              float2 uv : TEXCOORD0; uint4 bones : BLENDINDICES; float4 weights : BLENDWEIGHT; };
struct VSOut { float4 clip : SV_Position; float3 worldNormal : NORMAL1; float2 uv : TEXCOORD0; };

[shader("vertex")]
VSOut vsMain(VSIn i, uint drawId : SV_InstanceID) {
    DrawData d = draws[drawId];
    float4x4 skin = palette[d.paletteBase + i.bones.x] * i.weights.x
                  + palette[d.paletteBase + i.bones.y] * i.weights.y
                  + palette[d.paletteBase + i.bones.z] * i.weights.z
                  + palette[d.paletteBase + i.bones.w] * i.weights.w;
    float4 p = mul(skin, float4(i.position, 1.0));
    float3 n = mul(skin, float4(i.normal, 0.0)).xyz;
    VSOut o;
    float4 world = mul(d.model, p);
    o.clip = mul(frame.viewProj, world);
    o.worldNormal = normalize(mul(d.model, float4(n, 0.0)).xyz);
    o.uv = i.uv;
    return o;
}
