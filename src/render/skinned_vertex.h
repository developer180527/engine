#pragma once

#include "render/gpu.h"
#include <cstdint>

// Extended vertex format for skinned meshes.
// Same as Vertex but adds 4 bone indices + 4 blend weights.
// 48 (base) + 4 (joints) + 16 (weights) = 68 bytes per vertex.
struct SkinnedVertex {
    float   position[3];  // 12 bytes
    float   normal[3];    // 12 bytes
    float   tangent[4];   // 16 bytes
    float   uv[2];        //  8 bytes
    uint8_t joints[4];    //  4 bytes  (bone indices, max 256 bones)
    float   weights[4];   // 16 bytes  (blend weights, sum to 1.0)

    // See Vertex::kGpuFormat — the descriptor lives in render/gpu.cpp.
    static constexpr gpu::VertexFormat kGpuFormat = gpu::VertexFormat::Skinned;
};
