#pragma once
// ── NavService — engine navigation (Recast build + Detour runtime query) ─────
// Engine-owned navigation infrastructure, the same shape as physics/audio: the
// runtime bakes a navmesh from STATIC collision geometry (world-space triangles)
// once, then answers path queries every tick. Kits never see Recast/Detour —
// they go through the nav C-API group. One navmesh per world for now (solo
// tile); tiled/streaming comes when level size demands it.
#include <cstdint>

namespace nav {

// Recast bake parameters. Defaults suit a ~human-sized agent at metric scale.
struct BuildConfig {
    float cellSize      = 0.30f;   // xz voxel size (smaller = finer, slower)
    float cellHeight    = 0.20f;   // y voxel size
    float agentRadius   = 0.30f;   // walkable area eroded by this
    float agentHeight   = 1.80f;   // headroom needed to stand
    float agentMaxClimb = 0.40f;   // step-up height
    float agentMaxSlope = 45.0f;   // degrees
    float edgeMaxLen    = 12.0f;
    float edgeMaxError  = 1.3f;
    float regionMinSize = 8.0f;    // cull specks smaller than this (voxels)
    float regionMergeSz = 20.0f;
    float detailSampleDist    = 6.0f;
    float detailSampleMaxErr  = 1.0f;
    int   maxVertsPerPoly     = 6;
};

class NavService {
public:
    NavService();
    ~NavService();
    NavService(const NavService&)            = delete;
    NavService& operator=(const NavService&) = delete;

    // Bake a navmesh from world-space triangles (verts = xyz * vertCount,
    // tris = 3 indices * triCount). Replaces any existing mesh. False on
    // failure (empty/degenerate input, or Recast error) — ready() stays false.
    bool build(const float* verts, int vertCount,
               const int* tris, int triCount, const BuildConfig& cfg = {});

    bool ready() const;
    void clear();

    // Straight-path waypoints from start to end, world-space, written as
    // xyz triples into out (capacity maxPoints). Returns the waypoint count
    // (0 = no path / not ready). out[0] is the projected start, out[n-1] the
    // reachable point closest to end.
    int findPath(const float start[3], const float end[3],
                 float* out, int maxPoints) const;

    // Nearest point ON the navmesh to p (fills out[3]). False if none within
    // the search box — use to snap an agent/target onto walkable ground.
    bool projectPoint(const float p[3], float out[3]) const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace nav
