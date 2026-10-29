// Study 008 spike: a G6-shaped compute pass. Frustum-cull one sphere per object,
// append the survivors' draw ids, and count them into an indirect-args buffer.
struct Sphere { float4 centerRadius; };
// instanceCount is the atomic counter. WGSL allows atomics only on atomic<u32>
// types, so it is declared Atomic<uint>, not a plain uint (spike finding).
struct DrawArgs { uint indexCount; Atomic<uint> instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
struct Cull { float4 planes[6]; uint objectCount; uint indexCount; uint2 pad; };
[[vk::binding(0, 0)]] StructuredBuffer<Sphere>     spheres;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>     visible;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawArgs> args;
[[vk::binding(3, 0)]] ConstantBuffer<Cull>         cull;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= cull.objectCount) return;
    float4 s = spheres[i].centerRadius;
    for (uint p = 0; p < 6; ++p)
        if (dot(cull.planes[p].xyz, s.xyz) + cull.planes[p].w < -s.w) return;
    uint slot = args[0].instanceCount.add(1);
    visible[slot] = i;
}
