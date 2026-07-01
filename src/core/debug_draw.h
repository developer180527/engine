#pragma once
// ── DebugDraw — GPU-free immediate-mode debug-line collector ─────────────────
// Kits/plugins queue shapes via the engineDraw* C API; the renderer drains this
// into a single line pass each frame, and it's cleared at the start of the next
// frame. Everything is LINES — shapes tessellate to segments on the CPU here, so
// this header stays free of bgfx (it only produces vertices).
#include <bx/math.h>
#include <cstdint>
#include <vector>
#include <cmath>

namespace dbg {

struct DebugVertex { float x, y, z; uint32_t abgr; };   // matches the line vertex layout

// Pack 0..1 RGBA into bgfx's ABGR8. `a` defaults opaque.
inline uint32_t packRGBA(float r, float g, float b, float a = 1.0f) {
    auto c = [](float v){ return (uint32_t)(bx::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

class DebugDraw {
public:
    void clear()             { m_verts.clear(); }
    bool empty()       const { return m_verts.empty(); }
    const std::vector<DebugVertex>& vertices() const { return m_verts; }

    void line(const bx::Vec3& a, const bx::Vec3& b, uint32_t abgr) {
        m_verts.push_back({ a.x, a.y, a.z, abgr });
        m_verts.push_back({ b.x, b.y, b.z, abgr });
    }

    void box(const bx::Vec3& c, const bx::Vec3& he, uint32_t abgr) {
        // corner i: bit0=+x, bit1=+y, bit2=+z (bx::Vec3 has no default ctor).
        const bx::Vec3 p[8] = {
            { c.x-he.x, c.y-he.y, c.z-he.z }, { c.x+he.x, c.y-he.y, c.z-he.z },
            { c.x-he.x, c.y+he.y, c.z-he.z }, { c.x+he.x, c.y+he.y, c.z-he.z },
            { c.x-he.x, c.y-he.y, c.z+he.z }, { c.x+he.x, c.y-he.y, c.z+he.z },
            { c.x-he.x, c.y+he.y, c.z+he.z }, { c.x+he.x, c.y+he.y, c.z+he.z },
        };
        static const int e[12][2] = {
            {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4}, {0,4},{1,5},{2,6},{3,7} };
        for (auto& ed : e) line(p[ed[0]], p[ed[1]], abgr);
    }

    // Circle in the plane whose normal is `axis`.
    void circle(const bx::Vec3& c, const bx::Vec3& axis, float r, uint32_t abgr, int seg = 24) {
        const bx::Vec3 n = bx::normalize(axis);
        const bx::Vec3 t = (std::fabs(n.y) < 0.99f)
            ? bx::normalize(bx::cross(n, bx::Vec3{0,1,0}))
            : bx::normalize(bx::cross(n, bx::Vec3{1,0,0}));
        const bx::Vec3 b = bx::cross(n, t);
        bx::Vec3 prev{0,0,0};
        for (int i = 0; i <= seg; ++i) {
            const float a = (float)i / (float)seg * 6.2831853f;
            const bx::Vec3 p = bx::add(c, bx::add(bx::mul(t, std::cos(a) * r),
                                                  bx::mul(b, std::sin(a) * r)));
            if (i > 0) line(prev, p, abgr);
            prev = p;
        }
    }
    void disk(const bx::Vec3& c, const bx::Vec3& normal, float r, uint32_t abgr) {
        circle(c, normal, r, abgr);
    }
    void sphere(const bx::Vec3& c, float r, uint32_t abgr) {   // 3 great circles
        circle(c, bx::Vec3{1,0,0}, r, abgr);
        circle(c, bx::Vec3{0,1,0}, r, abgr);
        circle(c, bx::Vec3{0,0,1}, r, abgr);
    }

private:
    std::vector<DebugVertex> m_verts;
};

} // namespace dbg
