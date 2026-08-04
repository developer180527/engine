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

    // ONE sqrt, deliberately. The obvious form takes four — three basis-row
    // lengths for the scale, plus the local diagonal — and this function runs
    // per item per frustum, so twice per item per frame with a shadow caster.
    // At 50 000 objects those four sqrts were the largest single cost in the
    // whole cull (2.4 ms of 3.8 for the camera pass, plus 1.8 in the shadow one).
    //
    // Both reductions are exact, not approximations:
    //   * max(sqrt(a), sqrt(b), sqrt(c)) == sqrt(max(a, b, c)) — sqrt is
    //     monotonic, so compare the SQUARED row lengths and take one root;
    //   * diag * maxScale == sqrt(diagSq * maxScaleSq), folding the two roots
    //     into one multiply under a single sqrt.
    // Floating-point reassociation means the last bits can differ from the
    // four-sqrt form; the result is a conservative radius either way, and
    // equivalence is asserted within tolerance in render_world_test.
    auto rowLenSq = [&](int i) {
        return m[i]*m[i] + m[i+1]*m[i+1] + m[i+2]*m[i+2];
    };
    const float maxScaleSq = std::max({ rowLenSq(0), rowLenSq(4), rowLenSq(8) });
    const float diagSq = localSize.x * localSize.x +
                         localSize.y * localSize.y +
                         localSize.z * localSize.z;
    s.radius = 0.5f * std::sqrt(diagSq * maxScaleSq);
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
