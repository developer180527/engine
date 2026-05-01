#pragma once

#include <bx/math.h>

// Transform component.
//
// Position, rotation, and scale of an entity in 3D space. This is the most
// fundamental component in the engine — most entities will have one.
//
// Rotation is stored as a quaternion (4 floats: x, y, z, w) rather than Euler
// angles. Reasons:
//   - No gimbal lock under any orientation
//   - Composition is well-behaved (just multiply)
//   - Smooth interpolation via slerp
//   - Numerically stable when accumulating small rotations over time
//
// The inspector UI converts to/from Euler angles for human-friendly editing,
// but the storage is always quaternion. This conversion happens at the UI
// boundary, not in this struct.
//
// Default values represent the identity transform: at origin, no rotation,
// unit scale. An entity with a default Transform sits at the world origin.
struct Transform {
    bx::Vec3 position { 0.0f, 0.0f, 0.0f };

    // Identity quaternion: (x=0, y=0, z=0, w=1) means "no rotation".
    // bx::Quaternion is { float x, y, z, w; }.
    bx::Quaternion rotation { 0.0f, 0.0f, 0.0f, 1.0f };

    bx::Vec3 scale { 1.0f, 1.0f, 1.0f };

    // Compose this transform into a 4x4 model matrix in row-major order
    // (bgfx convention). Order of operations: scale, then rotate, then translate.
    // bx::mtxFromSrt does exactly this in one call.
    void getMatrix(float out[16]) const {
        bx::mtxFromSrt(
            out,
            scale.x,    scale.y,    scale.z,
            rotation.x, rotation.y, rotation.z, rotation.w,
            position.x, position.y, position.z
        );
    }
};
