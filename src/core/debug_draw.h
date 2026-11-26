#pragma once
// ── DebugDraw — GPU-free immediate-mode debug-line collector ─────────────────
// Kits/plugins queue shapes via the engineDraw* C API; the renderer drains this
// into a single line pass each frame, and it's cleared at the start of the next
// frame. Everything is LINES — shapes tessellate to segments on the CPU here.
//
// An ENGINE service, between providers (WO-087, provider-abi.md rule 9): physics,
// scripts and kits draw into it without knowing which renderer runs, and the
// renderer reads it without knowing who drew. So it names nothing of any
// provider's: its own point type and its own colour layout (RGBA, one byte each,
// in that order), no bgfx, no bx. Each renderer converts to its own vertex
// format when it drains the lines.
#include <cmath>
#include <cstdint>
#include <vector>

namespace dbg {

struct Vec3  { float x, y, z; };
// The engine's colour: R, G, B, A bytes, in that order in memory.
struct Color { uint8_t r = 255, g = 255, b = 255, a = 255; };
static_assert(sizeof(Color) == 4, "Color is four bytes, RGBA");

struct DebugVertex { float x, y, z; Color color; };

// 0..1 RGBA to a Color. `a` defaults opaque; values outside 0..1 are clamped,
// and NaN reads as 0.
inline Color packRGBA(float r, float g, float b, float a = 1.0f) {
    auto c = [](float v) -> uint8_t {
        if (!(v > 0.0f)) return 0;
        if (v >= 1.0f) return 255;
        return (uint8_t)(v * 255.0f + 0.5f);
    };
    return Color{c(r), c(g), c(b), c(a)};
}

class DebugDraw {
public:
    void clear()             { m_verts.clear(); }
    bool empty()       const { return m_verts.empty(); }
    const std::vector<DebugVertex>& vertices() const { return m_verts; }

    void line(const Vec3& a, const Vec3& b, Color c) {
        m_verts.push_back({ a.x, a.y, a.z, c });
        m_verts.push_back({ b.x, b.y, b.z, c });
    }

    void box(const Vec3& c, const Vec3& he, Color col) {
        // corner i: bit0=+x, bit1=+y, bit2=+z.
        const Vec3 p[8] = {
            { c.x-he.x, c.y-he.y, c.z-he.z }, { c.x+he.x, c.y-he.y, c.z-he.z },
            { c.x-he.x, c.y+he.y, c.z-he.z }, { c.x+he.x, c.y+he.y, c.z-he.z },
            { c.x-he.x, c.y-he.y, c.z+he.z }, { c.x+he.x, c.y-he.y, c.z+he.z },
            { c.x-he.x, c.y+he.y, c.z+he.z }, { c.x+he.x, c.y+he.y, c.z+he.z },
        };
        static const int e[12][2] = {
            {0,1},{1,3},{3,2},{2,0}, {4,5},{5,7},{7,6},{6,4}, {0,4},{1,5},{2,6},{3,7} };
        for (auto& ed : e) line(p[ed[0]], p[ed[1]], col);
    }

    // Circle in the plane whose normal is `axis`.
    void circle(const Vec3& c, const Vec3& axis, float r, Color col, int seg = 24) {
        const Vec3 n = normalize(axis);
        const Vec3 t = (std::fabs(n.y) < 0.99f) ? normalize(cross(n, Vec3{0, 1, 0}))
                                                : normalize(cross(n, Vec3{1, 0, 0}));
        const Vec3 b = cross(n, t);
        Vec3 prev{0, 0, 0};
        for (int i = 0; i <= seg; ++i) {
            const float a = (float)i / (float)seg * 6.2831853f;
            const float ca = std::cos(a) * r, sa = std::sin(a) * r;
            const Vec3 p{ c.x + t.x * ca + b.x * sa, c.y + t.y * ca + b.y * sa, c.z + t.z * ca + b.z * sa };
            if (i > 0) line(prev, p, col);
            prev = p;
        }
    }
    void disk(const Vec3& c, const Vec3& normal, float r, Color col) {
        circle(c, normal, r, col);
    }
    void sphere(const Vec3& c, float r, Color col) {   // 3 great circles
        circle(c, Vec3{1, 0, 0}, r, col);
        circle(c, Vec3{0, 1, 0}, r, col);
        circle(c, Vec3{0, 0, 1}, r, col);
    }

private:
    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    static Vec3 normalize(const Vec3& v) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 0.0f ? Vec3{ v.x / l, v.y / l, v.z / l } : Vec3{0, 0, 0};
    }
    std::vector<DebugVertex> m_verts;
};

} // namespace dbg
