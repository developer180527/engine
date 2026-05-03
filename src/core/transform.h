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
// The inspector UI converts to/from Euler at the UI boundary; storage is
// always quaternion.
//
// Default values represent identity: at origin, no rotation, unit scale.
struct Transform {
    bx::Vec3       position { 0.0f, 0.0f, 0.0f };
    bx::Quaternion rotation { 0.0f, 0.0f, 0.0f, 1.0f };
    bx::Vec3       scale    { 1.0f, 1.0f, 1.0f };

    // Compose into a 4x4 model matrix in bgfx row-major convention.
    //
    // Standard SRT order: vertex is first scaled, then rotated, then
    // translated. In matrix form for row vectors: out = S * R * T.
    //
    // We build this manually because:
    //   - bx::mtxSRT takes Euler angles, not a quaternion
    //   - bx::mtxFromQuaternion(out, q, t) bakes translation INTO the
    //     rotation result in a way that combines weirdly with scale
    //     (would scale the translation too)
    //
    // So: build R alone, build S, build T, multiply in order S*R*T.
    // After this composition, the matrix's translation row (m[12..14])
    // equals position.{x,y,z} exactly — independent of scale or rotation.
    // That property is what lets the gizmo extract position by reading
    // those three floats directly.
    void getMatrix(float out[16]) const {
        // Pure rotation matrix from the quaternion. mtxFromQuaternion's
        // single-arg overload writes the rotation with a zero translation row.
        float rotMtx[16];
        bx::mtxFromQuaternion(rotMtx, rotation);

        float scaleMtx[16];
        bx::mtxScale(scaleMtx, scale.x, scale.y, scale.z);

        // SR = S * R
        float sr[16];
        bx::mtxMul(sr, scaleMtx, rotMtx);

        // out = SR with translation row set to position. We can't simply
        // multiply by a translation matrix because in row-vector convention
        // S*R*T puts T's translation row into m[12..14] of the result —
        // exactly what we want. Build T explicitly and multiply.
        float transMtx[16];
        bx::mtxTranslate(transMtx, position.x, position.y, position.z);

        bx::mtxMul(out, sr, transMtx);
    }
};
