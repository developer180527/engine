// Study 008 spike: the smallest shader, used to RENDER on Metal (run_metal.swift).
struct VSOut { float4 pos : SV_Position; float3 color : COLOR; };
[shader("vertex")]
VSOut vsMain(uint vid : SV_VertexID) {
    float2 p[3] = { float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5) };
    float3 c[3] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1) };
    VSOut o; o.pos = float4(p[vid], 0.0, 1.0); o.color = c[vid]; return o;
}
[shader("fragment")]
float4 fsMain(VSOut i) : SV_Target { return float4(i.color, 1.0); }
