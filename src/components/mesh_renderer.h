#pragma once

#include "core/handle.h"

// MeshRenderer component.
//
// Tells the renderer "this entity should be drawn with this mesh." The
// renderer iterates view<Transform, MeshRenderer> and submits one draw call
// per matching entity.
//
// An entity needs both a Transform and a MeshRenderer to be rendered. A
// Transform alone (a "pivot point") doesn't render. A MeshRenderer alone
// (without a position in the world) wouldn't make sense.
//
// Currently holds only a mesh handle. As the engine grows, this will gain:
//   - MaterialHandle (which shader/uniforms to use)
//   - Layer flags (rendering layers, visibility groups)
//   - bool castsShadows / receivesShadows
//   - LOD level overrides
// For milestone 4, just the mesh suffices — every entity uses the same shader.
struct MeshRenderer {
    MeshHandle mesh;
};
