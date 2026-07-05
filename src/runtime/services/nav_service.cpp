// ── NavService implementation ────────────────────────────────────────────────
// A textbook Recast "solo mesh" bake (heightfield → compact → regions →
// contours → poly mesh → detail → Detour navmesh) plus a Detour query object.
// The intermediate Recast structures are scratch — only the Detour navmesh +
// query survive the build. See recastnavigation's Sample_SoloMesh for the
// canonical version of this pipeline.
#include "runtime/services/nav_service.h"

#include <cmath>
#include <cstring>

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourCommon.h>

#include "core/logger.h"

namespace nav {

namespace {
constexpr int   kMaxPolyPath   = 256;   // Detour corridor cap per query
constexpr float kSearchExt[3]  = {2.0f, 4.0f, 2.0f};  // findNearestPoly box
constexpr unsigned short kFlagWalk = 0x01;
} // namespace

struct NavService::Impl {
    dtNavMesh*      mesh  = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter   filter;

    ~Impl() { reset(); }
    void reset() {
        if (query) { dtFreeNavMeshQuery(query); query = nullptr; }
        if (mesh)  { dtFreeNavMesh(mesh);       mesh  = nullptr; }
    }
};

NavService::NavService() : m_impl(new Impl) {
    m_impl->filter.setIncludeFlags(kFlagWalk);
    m_impl->filter.setExcludeFlags(0);
}
NavService::~NavService() { delete m_impl; }

bool NavService::ready() const { return m_impl->mesh && m_impl->query; }
void NavService::clear()       { m_impl->reset(); }

bool NavService::build(const float* verts, int vertCount,
                       const int* tris, int triCount, const BuildConfig& c) {
    m_impl->reset();
    if (!verts || !tris || vertCount < 3 || triCount < 1) {
        LOG_WARN("Nav", "build skipped — empty/degenerate geometry (%d verts, %d tris)",
                 vertCount, triCount);
        return false;
    }

    rcContext ctx(false);   // no timers

    rcConfig cfg{};
    cfg.cs                     = c.cellSize;
    cfg.ch                     = c.cellHeight;
    cfg.walkableSlopeAngle     = c.agentMaxSlope;
    cfg.walkableHeight         = (int)std::ceil (c.agentHeight   / cfg.ch);
    cfg.walkableClimb          = (int)std::floor(c.agentMaxClimb / cfg.ch);
    cfg.walkableRadius         = (int)std::ceil (c.agentRadius   / cfg.cs);
    cfg.maxEdgeLen             = (int)(c.edgeMaxLen / c.cellSize);
    cfg.maxSimplificationError = c.edgeMaxError;
    cfg.minRegionArea          = (int)rcSqr(c.regionMinSize);
    cfg.mergeRegionArea        = (int)rcSqr(c.regionMergeSz);
    cfg.maxVertsPerPoly        = c.maxVertsPerPoly;
    cfg.detailSampleDist       = c.detailSampleDist < 0.9f ? 0 : c.cellSize * c.detailSampleDist;
    cfg.detailSampleMaxError   = c.cellHeight * c.detailSampleMaxErr;

    rcCalcBounds(verts, vertCount, cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    // Reject degenerate input (NaN/Inf coords, or absurd extents) BEFORE Recast
    // rasterizes a giant heightfield and hangs/OOMs the process. NaN bounds make
    // rcCalcGridSize produce garbage grid dims — cap the cell count too.
    if (!std::isfinite(cfg.bmin[0]) || !std::isfinite(cfg.bmin[1]) || !std::isfinite(cfg.bmin[2]) ||
        !std::isfinite(cfg.bmax[0]) || !std::isfinite(cfg.bmax[1]) || !std::isfinite(cfg.bmax[2]) ||
        cfg.width <= 0 || cfg.height <= 0 ||
        (int64_t)cfg.width * (int64_t)cfg.height > 64'000'000) {
        LOG_WARN("Nav", "build rejected — degenerate bounds / %dx%d grid too large",
                 cfg.width, cfg.height);
        return false;
    }

    // Scratch structures — freed at the end regardless of success.
    rcHeightfield*        solid = rcAllocHeightfield();
    rcCompactHeightfield* chf   = rcAllocCompactHeightfield();
    rcContourSet*         cset  = rcAllocContourSet();
    rcPolyMesh*           pmesh = rcAllocPolyMesh();
    rcPolyMeshDetail*     dmesh = rcAllocPolyMeshDetail();
    unsigned char*        areas = new unsigned char[triCount];
    unsigned char*        navData = nullptr;
    bool ok = false;

    auto fail = [&](const char* why) {
        LOG_ERROR("Nav", "build failed: %s", why);
        return false;
    };

    do {
        if (!solid || !chf || !cset || !pmesh || !dmesh || !areas) { fail("alloc"); break; }
        if (!rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height,
                                 cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) { fail("heightfield"); break; }

        std::memset(areas, 0, (size_t)triCount);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, vertCount,
                                tris, triCount, areas);
        if (!rcRasterizeTriangles(&ctx, verts, vertCount, tris, areas, triCount,
                                  *solid, cfg.walkableClimb)) { fail("rasterize"); break; }

        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

        if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb,
                                       *solid, *chf)) { fail("compact"); break; }
        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) { fail("erode"); break; }
        if (!rcBuildDistanceField(&ctx, *chf)) { fail("distfield"); break; }
        if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea,
                            cfg.mergeRegionArea)) { fail("regions"); break; }
        if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError,
                             cfg.maxEdgeLen, *cset)) { fail("contours"); break; }
        if (cset->nconts == 0) { fail("no contours (geometry not walkable?)"); break; }
        if (!rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) { fail("polymesh"); break; }
        if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist,
                                   cfg.detailSampleMaxError, *dmesh)) { fail("detail"); break; }

        // Tag every walkable poly with our single walk flag for the filter.
        for (int i = 0; i < pmesh->npolys; ++i)
            if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->flags[i] = kFlagWalk;

        dtNavMeshCreateParams p{};
        p.verts = pmesh->verts; p.vertCount = pmesh->nverts;
        p.polys = pmesh->polys; p.polyAreas = pmesh->areas; p.polyFlags = pmesh->flags;
        p.polyCount = pmesh->npolys; p.nvp = pmesh->nvp;
        p.detailMeshes = dmesh->meshes; p.detailVerts = dmesh->verts;
        p.detailVertsCount = dmesh->nverts; p.detailTris = dmesh->tris;
        p.detailTriCount = dmesh->ntris;
        p.walkableHeight = c.agentHeight; p.walkableRadius = c.agentRadius;
        p.walkableClimb = c.agentMaxClimb;
        rcVcopy(p.bmin, pmesh->bmin); rcVcopy(p.bmax, pmesh->bmax);
        p.cs = cfg.cs; p.ch = cfg.ch; p.buildBvTree = true;

        int navDataSize = 0;
        if (!dtCreateNavMeshData(&p, &navData, &navDataSize)) { fail("dtCreateNavMeshData"); break; }

        m_impl->mesh = dtAllocNavMesh();
        if (!m_impl->mesh) { fail("alloc navmesh"); break; }
        if (dtStatusFailed(m_impl->mesh->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
            fail("navmesh init"); break;
        }
        navData = nullptr;   // owned by the navmesh now (DT_TILE_FREE_DATA)

        m_impl->query = dtAllocNavMeshQuery();
        if (!m_impl->query || dtStatusFailed(m_impl->query->init(m_impl->mesh, 2048))) {
            fail("query init"); break;
        }
        ok = true;
    } while (false);

    rcFreeHeightField(solid);
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);
    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);
    delete[] areas;
    if (navData) dtFree(navData);
    if (!ok) m_impl->reset();
    else LOG_SUCCESS("Nav", "navmesh baked — %d polys from %d tris",
                     m_impl->mesh ? 1 : 0, triCount);
    return ok;
}

bool NavService::projectPoint(const float p[3], float out[3]) const {
    if (!ready()) return false;
    dtPolyRef ref = 0;
    const dtStatus s = m_impl->query->findNearestPoly(p, kSearchExt,
                                                      &m_impl->filter, &ref, out);
    return dtStatusSucceed(s) && ref != 0;
}

int NavService::findPath(const float start[3], const float end[3],
                         float* out, int maxPoints) const {
    if (!ready() || maxPoints < 1) return 0;
    auto* q = m_impl->query;
    const dtQueryFilter& f = m_impl->filter;

    dtPolyRef startRef = 0, endRef = 0;
    float startPt[3], endPt[3];
    q->findNearestPoly(start, kSearchExt, &f, &startRef, startPt);
    q->findNearestPoly(end,   kSearchExt, &f, &endRef,   endPt);
    if (!startRef || !endRef) return 0;

    dtPolyRef poly[kMaxPolyPath];
    int npoly = 0;
    if (dtStatusFailed(q->findPath(startRef, endRef, startPt, endPt, &f,
                                   poly, &npoly, kMaxPolyPath)) || npoly == 0)
        return 0;

    // Detour caps straight-path at its own array; clamp to the caller's buffer.
    int cap = maxPoints < kMaxPolyPath ? maxPoints : kMaxPolyPath;
    unsigned char sflags[kMaxPolyPath];
    dtPolyRef     srefs[kMaxPolyPath];
    int nstraight = 0;
    if (dtStatusFailed(q->findStraightPath(startPt, endPt, poly, npoly,
                                           out, sflags, srefs, &nstraight, cap, 0)))
        return 0;
    return nstraight;
}

} // namespace nav
