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
    // translated. In matrix form for row vectors: out = S * R * T. After it,
    // the translation row (m[12..14]) equals position exactly, independent of
    // scale or rotation — the property that lets the gizmo read position
    // straight out of those three floats.
    //
    // WRITTEN OUT DIRECTLY, not as bx::mtxMul(bx::mtxMul(S, R), T). Both forms
    // give the same 16 floats; the multiply form spends 128 multiplies and
    // 96 adds computing them, and three quarters of that arithmetic is against
    // structural zeros and ones — S is diagonal and T is the identity with one
    // row replaced. This is the most-called function in the engine (every
    // entity, every frame, plus physics sync, animation and gizmos), and it was
    // measurably expensive: it is the bulk of the extraction pass's remaining
    // cost at 20 000 objects.
    //
    // The algebra, so the constants below are checkable rather than magic. With
    // row vectors, S = diag(sx, sy, sz, 1) scales R's ROWS: (S*R)[i][j] =
    // s_i * R[i][j]. Multiplying that by T then leaves rows 0..2 untouched
    // (their 4th element is 0, so T's translation row contributes nothing) and
    // replaces row 3 with T's, which is position. Hence: scale each rotation
    // row, drop position in the last row, done.
    //
    // Equivalence to the reference two-multiply form is asserted over randomised
    // transforms in tests/extract_partition_test.cpp — it is not an eyeballed
    // refactor.
    void getMatrix(float out[16]) const {
        // Rotation only. The single-argument overload writes a zero translation
        // row, which is why the rows below can be scaled independently.
        float r[16];
        bx::mtxFromQuaternion(r, rotation);

        out[ 0] = r[ 0] * scale.x; out[ 1] = r[ 1] * scale.x;
        out[ 2] = r[ 2] * scale.x; out[ 3] = 0.0f;

        out[ 4] = r[ 4] * scale.y; out[ 5] = r[ 5] * scale.y;
        out[ 6] = r[ 6] * scale.y; out[ 7] = 0.0f;

        out[ 8] = r[ 8] * scale.z; out[ 9] = r[ 9] * scale.z;
        out[10] = r[10] * scale.z; out[11] = 0.0f;

        out[12] = position.x; out[13] = position.y; out[14] = position.z;
        out[15] = 1.0f;
    }
};
