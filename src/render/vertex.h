#pragma once

#include <bgfx/bgfx.h>

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

    // bgfx vertex layout descriptor. Built once at engine init, referenced
    // every time we create a vertex buffer. Static method so callers don't
    // have to hold a layout instance around.
    static bgfx::VertexLayout& layout() {
        static bgfx::VertexLayout layout = [] {
            bgfx::VertexLayout l;
            l.begin()
                .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                .end();
            return l;
        }();
        return layout;
    }
};
