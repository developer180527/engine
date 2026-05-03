#pragma once

#include <cstdint>
#include "vertex.h"

// Hardcoded cube primitive data.
//
// 24 vertices (4 per face × 6 faces) instead of 8 — cubes need duplicated
// vertices because each face has a different normal. Sharing 8 corners
// would force one normal per vertex, but a cube's corners are where 3 faces
// meet with 3 different normals.
//
// Indices use CCW winding viewed from OUTSIDE the cube, so back-face
// culling correctly hides the inside faces.

namespace primitive_cube {

inline constexpr Vertex kVertices[] = {
    // +X face (right) — normal points +X
    { { 1, -1, -1}, { 1, 0, 0}, {0, 0} },  // 0
    { { 1,  1, -1}, { 1, 0, 0}, {1, 0} },  // 1
    { { 1,  1,  1}, { 1, 0, 0}, {1, 1} },  // 2
    { { 1, -1,  1}, { 1, 0, 0}, {0, 1} },  // 3
    // -X face (left)
    { {-1, -1,  1}, {-1, 0, 0}, {0, 0} },  // 4
    { {-1,  1,  1}, {-1, 0, 0}, {1, 0} },  // 5
    { {-1,  1, -1}, {-1, 0, 0}, {1, 1} },  // 6
    { {-1, -1, -1}, {-1, 0, 0}, {0, 1} },  // 7
    // +Y face (top)
    { {-1,  1, -1}, { 0, 1, 0}, {0, 0} },  // 8
    { {-1,  1,  1}, { 0, 1, 0}, {1, 0} },  // 9
    { { 1,  1,  1}, { 0, 1, 0}, {1, 1} },  // 10
    { { 1,  1, -1}, { 0, 1, 0}, {0, 1} },  // 11
    // -Y face (bottom)
    { {-1, -1,  1}, { 0,-1, 0}, {0, 0} },  // 12
    { {-1, -1, -1}, { 0,-1, 0}, {1, 0} },  // 13
    { { 1, -1, -1}, { 0,-1, 0}, {1, 1} },  // 14
    { { 1, -1,  1}, { 0,-1, 0}, {0, 1} },  // 15
    // +Z face (front)
    { {-1, -1,  1}, { 0, 0, 1}, {0, 0} },  // 16
    { { 1, -1,  1}, { 0, 0, 1}, {1, 0} },  // 17
    { { 1,  1,  1}, { 0, 0, 1}, {1, 1} },  // 18
    { {-1,  1,  1}, { 0, 0, 1}, {0, 1} },  // 19
    // -Z face (back)
    { { 1, -1, -1}, { 0, 0,-1}, {0, 0} },  // 20
    { {-1, -1, -1}, { 0, 0,-1}, {1, 0} },  // 21
    { {-1,  1, -1}, { 0, 0,-1}, {1, 1} },  // 22
    { { 1,  1, -1}, { 0, 0,-1}, {0, 1} },  // 23
};

// Indices: CCW winding viewed from outside each face.
// Per face: triangle 1 = (0, 2, 1), triangle 2 = (0, 3, 2)
// where indices are local to the face's 4 vertices.
inline constexpr uint16_t kIndices[] = {
     0,  2,  1,    0,  3,  2,  // +X
     4,  6,  5,    4,  7,  6,  // -X
     8, 10,  9,    8, 11, 10,  // +Y
    12, 14, 13,   12, 15, 14,  // -Y
    16, 18, 17,   16, 19, 18,  // +Z
    20, 22, 21,   20, 23, 22,  // -Z
};

inline constexpr uint32_t kVertexCount = sizeof(kVertices) / sizeof(kVertices[0]);
inline constexpr uint32_t kIndexCount  = sizeof(kIndices)  / sizeof(kIndices[0]);

} // namespace primitive_cube
