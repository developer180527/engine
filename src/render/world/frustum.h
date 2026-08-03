#pragma once
// ── Frustum culling — pure math, no Mesh, no bgfx ───────────────────────────
//
// ONE concern: is this bounding volume outside the view?
//
// Lifted verbatim (behaviour-wise) out of ForwardPipeline::culled(), where it
// sat as a private static in a 442-line class and could not be tested. A
// conservative visibility test is exactly the kind of code that fails
// silently: too aggressive and geometry pops out of existence at the screen
// edge; too lax and it just costs performance. Neither shows up in a crash.
#include "core/math_types.h"

namespace rworld {

// World-space bounding sphere. Conservative on purpose — a sphere never
// under-estimates an AABB's extent, so the cull can drop things but never
// keep something it should have dropped by accident.
struct BoundingSphere {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float radius = 0.0f;
};

// Transform LOCAL bounds by a model matrix into a world sphere.
//
// The radius uses the LARGEST row length of the 3x3 as the scale. Using each
// axis separately would be wrong for a rotated non-uniform scale (the local
// axes no longer align with world axes after rotation), and using the average
// would under-estimate — which is the direction that makes geometry vanish.
BoundingSphere worldSphere(const float model[16],
                           const Vec3& localCenter, const Vec3& localSize);

// True when the sphere is entirely OUTSIDE at least one plane, i.e. it can be
// skipped. Planes are (a,b,c,d) with a normalized normal, pointing inward.
bool outsideFrustum(const BoundingSphere& s, const float planes[6][4]);

// Six inward-pointing, normalized planes from a combined view*proj matrix.
//
// Shared because there is more than one frustum in a frame. The camera's was
// computed inline in Renderer::buildView, so the SHADOW pass had no planes of its
// own and culled nothing — it submitted every item in the scene regardless of the
// light's frustum. A directional light's frustum is not the camera's (an object
// behind the camera can still cast a shadow INTO view), so the shadow pass needs
// its own planes from the LIGHT matrices, not a copy of the camera's.
//
// Row-major `viewProj`, i.e. bx::mtxMul(vp, view, proj).
void extractFrustumPlanes(const float viewProj[16], float planes[6][4]);

} // namespace rworld
