#pragma once

#include <bgfx/bgfx.h>

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

    static bgfx::VertexLayout& layout() {
        static bgfx::VertexLayout layout = [] {
            bgfx::VertexLayout l;
            l.begin()
                .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Normal,    3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Tangent,   4, bgfx::AttribType::Float)
                .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Indices,   4, bgfx::AttribType::Uint8, true)
                .add(bgfx::Attrib::Weight,    4, bgfx::AttribType::Float)
                .end();
            return l;
        }();
        return layout;
    }
};
