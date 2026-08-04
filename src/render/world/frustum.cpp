#include "render/world/frustum.h"

#include <algorithm>
#include <cmath>

namespace rworld {

BoundingSphere worldSphere(const float m[16],
                           const Vec3& localCenter, const Vec3& localSize) {
    BoundingSphere s;
    // Column-major (bx convention): the translation lives in m[12..14].
    s.x = m[0]*localCenter.x + m[4]*localCenter.y + m[8]*localCenter.z  + m[12];
    s.y = m[1]*localCenter.x + m[5]*localCenter.y + m[9]*localCenter.z  + m[13];
    s.z = m[2]*localCenter.x + m[6]*localCenter.y + m[10]*localCenter.z + m[14];

    // The EXACT conservative radius for a transformed AABB, in one sqrt.
    //
    // What this replaced, and why it was wrong (issues.md A1.1): the radius used
    // the largest basis-row length as "the scale". For a matrix straight out of
    // Transform::getMatrix that is exactly right — its linear part is
    // diag(scale) * orthonormal, so the row lengths ARE the singular values, and
    // a probe over all 8 corners of a rotated non-uniform SRT shows zero overrun.
    // But a matrix composed through a HIERARCHY is not of that form. A 45-degree
    // child under a parent scaled 4x on one axis gave a sphere that missed its own
    // corners by 0.35 units — and an under-estimated sphere means the cull drops
    // geometry that is genuinely visible, which shows up as objects popping out of
    // existence near the screen edge and never as a crash.
    //
    // Instead of bounding the matrix, bound the BOX. Under p' = p * M the world
    // half-extent along axis j is sum_i h_i * |M[i][j]| — the standard absolute-
    // value transform of an AABB — and the sphere that contains that box has
    // radius |extent|. It is conservative by construction (it is the box's own
    // corner bound), and it is TIGHTER than the old form for the common case of
    // non-uniform scale, which multiplied the whole diagonal by the largest axis.
    // Same cost: nine multiply-adds and one square root.
    const float hx = 0.5f * localSize.x;
    const float hy = 0.5f * localSize.y;
    const float hz = 0.5f * localSize.z;
    const float ex = hx * std::fabs(m[0]) + hy * std::fabs(m[4]) + hz * std::fabs(m[8]);
    const float ey = hx * std::fabs(m[1]) + hy * std::fabs(m[5]) + hz * std::fabs(m[9]);
    const float ez = hx * std::fabs(m[2]) + hy * std::fabs(m[6]) + hz * std::fabs(m[10]);
    s.radius = std::sqrt(ex*ex + ey*ey + ez*ez);
    return s;
}

bool outsideFrustum(const BoundingSphere& s, const float planes[6][4]) {
    for (int p = 0; p < 6; ++p) {
        const float d = planes[p][0]*s.x + planes[p][1]*s.y
                      + planes[p][2]*s.z + planes[p][3];
        // Fully behind this plane by more than the radius -> cannot be visible.
        if (d < -s.radius) return true;
    }
    return false;
}


void extractFrustumPlanes(const float vp[16], float planes[6][4]) {
    auto setPlane = [&](int i, float a, float b, float c, float d) {
        float l = std::sqrt(a*a + b*b + c*c);
        if (l < 1e-6f) l = 1.0f;
        planes[i][0]=a/l; planes[i][1]=b/l; planes[i][2]=c/l; planes[i][3]=d/l;
    };
    setPlane(0, vp[3]+vp[0], vp[7]+vp[4], vp[11]+vp[8],  vp[15]+vp[12]);
    setPlane(1, vp[3]-vp[0], vp[7]-vp[4], vp[11]-vp[8],  vp[15]-vp[12]);
    setPlane(2, vp[3]+vp[1], vp[7]+vp[5], vp[11]+vp[9],  vp[15]+vp[13]);
    setPlane(3, vp[3]-vp[1], vp[7]-vp[5], vp[11]-vp[9],  vp[15]-vp[13]);
    setPlane(4, vp[2],       vp[6],       vp[10],        vp[14]);
    setPlane(5, vp[3]-vp[2], vp[7]-vp[6], vp[11]-vp[10], vp[15]-vp[14]);
}

} // namespace rworld
