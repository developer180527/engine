#pragma once
#include <bx/math.h>
#include <cstring>

// Engine math types for the render contract (and beyond). Vec3 reuses bx::Vec3
// — the type Transform already uses — so there's one vector type across the
// engine. NOTE: bx::Vec3 has no default constructor; any Vec3 member must
// carry a brace initializer.
using Vec3 = bx::Vec3;

// 16-byte aligned 4-vector — matches GPU vec4 / SIMD layout. Used for values
// that go straight into a uniform (e.g. camera position).
struct alignas(16) Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    const float* ptr() const { return &x; }
};

// Row-major 4x4 (bgfx convention). Thin POD over float[16] for type safety at
// API boundaries; .ptr() hands the raw array to bx / bgfx.
struct Mat4 {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float* ptr() const { return m; }
    float*       ptr()       { return m; }
    static Mat4 from(const float src[16]) { Mat4 r; std::memcpy(r.m, src, sizeof(r.m)); return r; }
};
