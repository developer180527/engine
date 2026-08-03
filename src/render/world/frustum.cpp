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

    // Length of each basis row = that axis's scale factor.
    auto rowLen = [&](int i) {
        return std::sqrt(m[i]*m[i] + m[i+1]*m[i+1] + m[i+2]*m[i+2]);
    };
    const float maxScale = std::max({ rowLen(0), rowLen(4), rowLen(8) });

    const float diag = std::sqrt(localSize.x * localSize.x +
                                 localSize.y * localSize.y +
                                 localSize.z * localSize.z);
    s.radius = diag * 0.5f * maxScale;
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
