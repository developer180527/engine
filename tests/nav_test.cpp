// ── nav_test — NavService bake + path-query gauntlet ────────────────────────
// Builds a navmesh from a ground quad with a WALL across the middle that blocks
// the straight line from A to B, then proves Detour routes the path AROUND it
// (through the gap) instead of through it. Pure CPU — no GPU, no engine boot —
// so it's a fast, deterministic regression for the whole Recast+Detour pipeline.
#include <cmath>
#include <cstdio>
#include <vector>

#include "runtime/services/nav_service.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                        \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("\n"); ++g_failures; }                \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Append an axis-aligned box [min,max] as 12 triangles into verts/tris.
static void addBox(std::vector<float>& v, std::vector<int>& t,
                   float x0, float y0, float z0, float x1, float y1, float z1) {
    const int b = (int)(v.size() / 3);
    const float p[8][3] = {
        {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},
        {x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},
    };
    for (auto& c : p) { v.push_back(c[0]); v.push_back(c[1]); v.push_back(c[2]); }
    const int f[12][3] = {
        {0,2,1},{0,3,2}, {4,5,6},{4,6,7},        // bottom, top
        {0,1,5},{0,5,4}, {1,2,6},{1,6,5},        // sides
        {2,3,7},{2,7,6}, {3,0,4},{3,4,7},
    };
    for (auto& tri : f) { t.push_back(b+tri[0]); t.push_back(b+tri[1]); t.push_back(b+tri[2]); }
}

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("nav_test: NavService bake + path query\n");

    // Ground: 20x20 quad on the XZ plane at y=0.
    std::vector<float> verts = {
        -10,0,-10,  10,0,-10,  10,0,10,  -10,0,10,
    };
    std::vector<int> tris = { 0,2,1, 0,3,2 };   // CCW → +Y normal (walkable up)
    // Wall across most of the width (x -10..2), leaving a GAP at x 2..10.
    // Tall enough (y 0..3) that no agent steps over it.
    addBox(verts, tris, -10.0f, 0.0f, -0.5f, 2.0f, 3.0f, 0.5f);

    nav::NavService nav;
    CHECK(!nav.ready(), "not ready before build");

    float start[3] = {-8, 0, -8};   // front-left
    float end[3]   = { 8, 0,  8};   // back-right; straight line crosses the wall
    float scratch[8*3];
    CHECK(nav.findPath(start, end, scratch, 8) == 0, "findPath on empty nav returns 0");

    const bool built = nav.build(verts.data(), (int)verts.size()/3,
                                 tris.data(), (int)tris.size()/3);
    CHECK(built && nav.ready(), "navmesh baked from ground + wall");

    // Project an off-mesh point down onto the ground.
    float proj[3];
    // A named local, not `(const float[3]){3, 1, 3}`. That is a C99 COMPOUND
    // LITERAL, which GCC and Clang accept in C++ as an extension and MSVC
    // rejects: "C4576: a parenthesized type followed by an initializer list is a
    // non-standard explicit type conversion syntax". Nothing is lost — the
    // temporary only had to live for the duration of the call.
    const float query[3] = {3, 1, 3};
    CHECK(nav.projectPoint(query, proj) && proj[1] < 1.0f,
          "projectPoint snaps onto the navmesh");

    // The path must route AROUND the wall: a straight shot would be 2 points;
    // detouring through the gap adds an intermediate corner on the gap side.
    float path[32*3];
    const int n = nav.findPath(start, end, path, 32);
    CHECK(n >= 2, "path found start->end (%d waypoints)", n);
    CHECK(n >= 3, "path BENT around the wall (>=1 intermediate corner)");

    bool detoured = false;
    for (int i = 1; i < n - 1; ++i)          // intermediate waypoints only
        if (path[i*3] > 1.5f) detoured = true;   // pulled toward the gap (x>2)
    CHECK(detoured, "an intermediate waypoint hugs the gap side (routed around, not through)");

    // Endpoints must be sane (near the requested start/end in XZ).
    const float dx = path[0] - start[0], dz = path[2] - start[2];
    CHECK(std::sqrt(dx*dx + dz*dz) < 2.0f, "path starts near the requested start");

    if (g_failures) { std::printf("nav_test: FAIL — %d failure(s)\n", g_failures); return 1; }
    std::printf("nav_test: PASS — navmesh routes around obstacles\n");
    return 0;
}
