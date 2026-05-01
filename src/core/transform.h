#pragma once

#include <bx/math.h>

// Transform component.
//
// Position, rotation, and scale of an entity in 3D space. The most fundamental
// component in the engine — most entities will have one.
//
// Rotation is stored as a quaternion rather than Euler angles. Reasons:
//   - No gimbal lock under any orientation
//   - Composition is well-behaved (just multiply)
//   - Smooth interpolation via slerp
//   - Numerically stable when accumulating small rotations over time
//
// The inspector UI converts to/from Euler angles for human-friendly editing,
// but storage is always quaternion. Conversion happens at the UI boundary,
// not in this struct.
//
// Default values represent identity: at origin, no rotation, unit scale.
struct Transform {
    bx::Vec3 position { 0.0f, 0.0f, 0.0f };

    // Identity quaternion: (x=0, y=0, z=0, w=1) means "no rotation".
    bx::Quaternion rotation { 0.0f, 0.0f, 0.0f, 1.0f };

    bx::Vec3 scale { 1.0f, 1.0f, 1.0f };

    // Compose this transform into a 4x4 model matrix.
    //
    // bx doesn't have a single SRT-from-quaternion call (mtxSRT takes Euler),
    // so we compose manually:
    //   1. Build a rotation+translation matrix from the quaternion + position
    //   2. Build a scale matrix
    //   3. Multiply: result = scale × rotation_translation
    //
    // For row-major matrices applied to row vectors (bgfx's convention), this
    // ordering produces the correct SRT transform: a vertex is first scaled,
    // then rotated, then translated when transformed by `out`.
    void getMatrix(float out[16]) const {
        float rotTrans[16];
        bx::mtxFromQuaternion(rotTrans, rotation, position);

        float scaleMtx[16];
        bx::mtxScale(scaleMtx, scale.x, scale.y, scale.z);

        bx::mtxMul(out, scaleMtx, rotTrans);
    }
};
