#pragma once

#include "render/gpu.h"

// Standard vertex format for engine-loaded meshes.
//
// Position + Normal + UV is the universal minimum for 3D rendering with
// lighting. Position is needed to render. Normal is needed for any lighting
// (Lambertian, PBR, even simple ambient hacks). UV is needed for textures.
//
// We store all three even though the current shader only uses position
// (and bakes color from a hardcoded palette). When milestone 7 adds lighting
// and milestone 8+ adds textures, the vertex format won't have to change —
// only the shader does.
//
// Layout is 32 bytes per vertex: 12 (pos) + 12 (normal) + 8 (uv). On modern
// GPUs this is a clean 2-cacheline stride, no padding waste.
struct Vertex {
    float position[3];  // x, y, z          12 bytes
    float normal[3];    // nx, ny, nz        12 bytes
    float tangent[4];   // xyz + handedness  16 bytes
    float uv[2];        // u, v in [0, 1]    8 bytes

    // Which GPU layout describes this struct. The layout DESCRIPTOR itself
    // lives in render/gpu.cpp: it is the backend's vocabulary, and keeping it
    // here put <bgfx/bgfx.h> into every file that names a Vertex — including
    // three asset importers that have no business knowing a graphics API
    // (docs/rhi/phases.md G1).
    //
    // The struct layout above and the descriptor there must agree; gpu.cpp
    // static_asserts the stride, which is the half a compiler can check.
    static constexpr gpu::VertexFormat kGpuFormat = gpu::VertexFormat::Standard;
};
