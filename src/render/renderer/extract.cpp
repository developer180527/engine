// ── Renderer: extraction — ECS world → RenderView ────────────────────────────
//
// ONE concern: turning entities into the flat POD arrays the pipeline consumes.
// This is the only place in the renderer that knows flecs, Transform, or
// MeshRenderer exist; everything downstream sees `RenderItem`/`LightItem` spans.
//
// IT IS ALSO THE HOT PATH, and that is why it is its own translation unit. Draw
// submission stopped being the bottleneck once instancing landed — a 20 000-object
// scene submits ONE draw call — and this is what replaced it. Render.extract, at
// 20 000 objects, over four changes:
//
//   15.2 ms  start
//    9.7 ms  optional/partitioned queries instead of per-entity lookups
//    8.0 ms  Transform::getMatrix composes SRT directly (was two 4x4 multiplies)
//    6.6 ms  SkinnedMesh as an optional term too
//    1.1 ms  chunked extraction on the job pool
//
// Scaling now (shadows on, same machine):
//
//   objects   extract   cull    frame
//     1 000   0.32 ms   0.35    8.3 (vsync)
//     5 000   0.50      0.34    8.3 (vsync)
//    20 000   1.13      0.52    8.2 (vsync)
//    50 000   2.83      0.78    8.2 (vsync)
//   100 000   5.41      1.37    9.0   (~111 fps)
//
// The lesson worth carrying, because it held for all four: almost none of the cost
// was arithmetic. It was per-entity work asking questions the query engine answers
// per ARCHETYPE (try_get<PrevTransform>, try_get<SkinnedMesh>, a redundant
// try_get<Transform>, target(ChildOf) + is_alive() + has<Transform>()), plus one
// function computing 16 floats with 128 multiplies. Three of the four fixes are in
// the query DECLARATIONS (renderer.h) and in transform.h, not in this loop. The same
// pattern then fixed Sim.prevSnapshot outside this subsystem, 12.7 ms -> 0.63.
//
// Measured NOT to matter, so nobody spends a day on them again: the three registry
// lookups are ~0.17 ms and reserving m_items up front is ~0.4 ms. Hand-written SIMD
// was NOT done, and after the cull was fixed too (issues.md R16 — 3.9 ms -> 0.88)
// extraction is once again the largest phase: 5.4 ms of a 7.8 ms render path at
// 100 000 objects. It is already parallel, so what remains is not work but
// STREAMING — 100 000 RenderItems, now 128 bytes each (two write-only fields removed,
// the rest repacked; world/issues.md A3). The cull no longer reads them at all.
//
// AND THAT IS ABOUT THE END OF THE CHEAP WINS, for a reason worth carrying: this loop
// is parallel across 12 cores, so a per-item saving divides by the core count before
// it reaches frame time. Deleting two registry lookups per item — measured at 0.17 ms
// per 20 000 items back when extraction was SERIAL — bought 0.18 ms at 100 000, not
// the ~0.85 ms the old number predicts. The next real lever is therefore not a
// narrower item or fewer instructions but NOT EXTRACTING AT ALL for entities that did
// not change (incremental extraction off flecs change detection), which is a design
// change and should start with measuring how static a typical frame's item set is.
//
// EXTRACTION ALSO BUILDS THE CULL'S WORKING SET (CullStreams): the world bounding
// sphere and the material+mesh half of the sort key, written here while the model
// matrix is still in registers — two streams, an interleaved 16-byte sphere plus a
// key (the layout is measured, see world/issues.md A2.P1). Not for this file's
// benefit — it costs ~0.2 ms at 50 000 objects — but because the sphere DOES NOT
// DEPEND ON THE CAMERA and was being rebuilt once per view, so the shadow pass was
// redoing every sphere the camera pass had just computed. That duplication is what
// disappeared (Render.shadow 0.25 -> 0.09 ms at 50 k). See world/issues.md A2.P1 for
// the full before/after, including the parts that did NOT get faster.
//
// The streams are parallel to m_items and must stay that way: the compaction pass
// moves both, or the cull reads one object's bounds for another's, silently.
//
// PARALLELISM, and the two things that make it safe rather than brave:
//   * A CHUNK IS COMPONENT DATA. Extraction slices archetypes into ranges of
//     contiguous arrays and the worker body touches no flecs handle at all — that
//     is only possible because PrevTransform and SkinnedMesh became query terms.
//     The registries it reads (mesh/material/texture/skeleton) are const lookups
//     into vectors, and each chunk writes a DISJOINT slice of m_items.
//   * ONE BODY, serial or parallel. Below kExtractParallelMin, or with no job pool
//     (headless tools, tests), the same extractChunk runs in a loop. A separate
//     serial implementation would be the one every test exercises and the parallel
//     one the version that ships; they would drift, invisibly.
// Parented items stay serial: they need an entity handle to walk ancestors, and
// they are a small minority of any scene.
//
// NOT sanitizer-verified at scene scale: ASan on the windowed host runs ~13 s per
// frame here, so the 20 000-object run under it was abandoned. The evidence is that
// every submit counter is byte-identical serial vs parallel at 1 k / 5 k / 20 k /
// 50 k — and those counters depend on every matrix, since culling reads them — plus
// a debug assert on the slice arithmetic. A TSan lane over a scene this size is
// still worth having.
//
// Two properties worth not breaking:
//   * Bounds are COPIED into the item, not read through the Mesh during culling.
//     Extraction is where a Mesh pointer is legitimately live; visibility runs
//     once per view (and shadow cascades multiply that), so it scans contiguous
//     PODs instead of chasing a pointer into a GPU-resource object per item per
//     view. It also keeps rworld:: free of bgfx, which is what makes culling
//     testable at all (tests/render_world_test.cpp).
//   * Queries are CACHED per world. The editor world's two are built at init;
//     the Play snapshot world's go through WorldQueryCache, whose entries the
//     runtime must drop when that world dies (resetWorldCaches) — a query that
//     outlives its world is a crash, not a leak.
#include "render/renderer.h"

#include <cassert>
#include <cmath>
#include <cstring>   // memmove, for the compaction pass
#include <limits>

#include <bx/math.h>

#include "core/logger.h"
#include "core/profiler.h"
#include "runtime/jobs/jobs.h"
#include "core/transform_utils.h"        // getWorldMatrixLerp
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "render/world/frustum.h"        // shared frustum-plane extraction
#include "render/world/lod.h"            // screen-height LOD selection

namespace {
// Items per job. Sized so one chunk is comfortably more than the ~10 µs the job
// facade asks for (measured ~0.3 µs per item, so 512 items is ~150 µs), while
// still giving a 20 000-object scene enough chunks to spread over every core.
constexpr uint32_t kExtractGrain = 512;
// Below this, threading loses: the parallelFor round trip costs more than the
// work. MEASURED — see the scaling table in this file's header.
constexpr uint32_t kExtractParallelMin = 2048;
} // namespace

RenderView Renderer::buildView(flecs::world& world, const float view[16],
                               const float proj[16], const RenderTarget& target,
                               bgfx::ViewId baseViewId) {
    ENGINE_PROFILE_SCOPE("Render.extract");
    RenderView rv;
    rv.view       = Mat4::from(view);
    rv.proj       = Mat4::from(proj);
    rv.target     = target;
    rv.baseViewId = baseViewId;

    rv.camPos = { -(view[12]*view[0] + view[13]*view[1] + view[14]*view[2]),
                  -(view[12]*view[4] + view[13]*view[5] + view[14]*view[6]),
                  -(view[12]*view[8] + view[13]*view[9] + view[14]*view[10]),
                  1.0f };

    // Shared with the shadow pass's LIGHT frustum — see rworld::extractFrustumPlanes.
    float vp[16]; bx::mtxMul(vp, view, proj);
    rworld::extractFrustumPlanes(vp, rv.frustum);

    // Everything about an item EXCEPT its world matrix, which is the only part
    // that differs between the parentless and parented sets.
    //
    // Takes no entity — deliberately. Every component it needs arrives from the
    // query, so this reads COMPONENT DATA and nothing else. That is what makes
    // the parentless path safe to run off the main thread (see the parallel
    // extraction below); an ECS handle in here would put flecs lookups on worker
    // threads, which is a different and much harder safety argument.
    auto extractItem = [&](const MeshRenderer& mr, const SkinnedMesh* skin,
                           RenderItem& it) -> bool {
        const Mesh* mesh = m_assets->getMesh(mr.mesh);
        if (!mesh) return false;
        it.mesh = mesh;
        MaterialHandle mh = mr.materialOverride.valid()
                            ? mr.materialOverride : mesh->material;
        it.material = mh;                    // fallback for submesh ranges
        it.meshKey = mr.mesh.id;
        it.matKey  = mh.id;

        it.hasBounds = mesh->hasBounds();    // copied, not borrowed — see header
        if (it.hasBounds) {
            it.boundsCenter = mesh->boundsCenter();
            it.boundsSize   = mesh->boundsSize();
        }

        // Skinned mesh: pass the bone palette to the pipeline. Another optional
        // query term rather than a try_get — worth ~1.4 ms per frame at 20 000
        // objects, because skinned and static entities live in different
        // archetypes and the query already knows which is which.
        if (skin && skin->hasSkinMatrices && m_skeletons) {
            const Skeleton* skel = m_skeletons->get(skin->skeleton);
            if (skel) {
                it.boneMatrices = skin->skinMatrices;
                it.boneCount    = skel->boneCount();
            }
        }
        return true;
    };

    // ── LOD: swap in a coarser mesh once the item's screen size is known ────
    //
    // Runs AFTER writeCullEntry, because the world bounding sphere it needs is
    // exactly what that call just produced — recomputing a radius here would
    // duplicate the one piece of per-item arithmetic this file works hardest to
    // do only once.
    //
    // It therefore has to REPAIR the sort key. writeCullEntry packed mesh and
    // material ids into keyBase, and changing which mesh draws invalidates both.
    // Leaving the key stale is not a cosmetic sorting problem: the submit loop
    // collapses runs of equal keys into ONE INSTANCED SUBMIT, so items at
    // different levels sharing a stale key would all be drawn with whichever
    // mesh the run started on. Hence keyBase is an out-parameter here rather
    // than something a caller might forget.
    //
    // The BOUNDS deliberately stay level 0's. A coarser mesh has slightly
    // different bounds, and letting them change with the level would make an
    // object's cull result flip as it crosses a threshold — objects popping in
    // and out at the screen edge, which is worse than a marginally conservative
    // sphere. It also keeps the LOD decision from feeding back into itself: the
    // screen height that chose the level is not altered by the choice.
    const float projYScale = proj[5];   // = 1 / tan(fovY/2); see world/lod.h
    auto applyLod = [&](const MeshRenderer& mr, const LodMesh& lod,
                        const CullSphere& sp, RenderItem& it, uint64_t& keyBase) {
        if (lod.count == 0) return;                  // inert chain
        // Level 0's cost, banked BEFORE a level is chosen — this is the
        // counterfactual the saving is measured against.
        const uint32_t fullTris = it.mesh ? it.mesh->indexCount / 3 : 0;
        m_lodTrisFull.fetch_add(fullTris, std::memory_order_relaxed);
        // Sentinel radii are not sizes: a missing mesh (< 0) or an unbounded one
        // (infinity) has no screen height to threshold against, so it stays at
        // full detail. Unbounded already means "missing data" to the cull, and
        // guessing a level for it would hide that.
        if (!(sp.r > 0.0f) || sp.r == std::numeric_limits<float>::infinity()) return;

        // Same view-space depth as the cull computes (visibility.cpp): row-vector
        // convention, magnitude so handedness cannot collapse it to zero.
        const float zv = sp.x * view[2] + sp.y * view[6] + sp.z * view[10] + view[14];
        const float h  = rworld::lodScreenHeight(sp.r, zv < 0.0f ? -zv : zv,
                                                 projYScale);

        uint8_t level = rworld::selectLod(lod.count, lod.coarsenBelow, h);
        // Walk back toward finer levels if the chosen one does not resolve. A
        // half-cooked chain must degrade to MORE detail, never to a hole in the
        // frame — and it is counted, because silently drawing level 0 everywhere
        // is indistinguishable from LOD not working at all.
        while (level > 0) {
            const MeshHandle mh = lod.mesh[level - 1];
            if (const Mesh* lm = m_assets->getMesh(mh)) {
                it.mesh    = lm;
                it.meshKey = mh.id;
                MaterialHandle mat = mr.materialOverride.valid()
                                     ? mr.materialOverride : lm->material;
                it.material = mat;
                it.matKey   = mat.id;
                keyBase = rworld::opaqueKeyBase(it.matKey, it.meshKey);
                break;
            }
            m_lodBroken.fetch_add(1, std::memory_order_relaxed);
            if (!m_warnedLodBroken.test_and_set(std::memory_order_relaxed))
                LOG_WARN("Renderer", "LOD level %u has no mesh — falling back to a "
                                     "finer level. The scene is drawing more "
                                     "triangles than it asked to; check that every "
                                     "level in the chain is cooked.",
                         (unsigned)level);
            --level;
        }
        m_lodCount[level].fetch_add(1, std::memory_order_relaxed);
        m_lodTrisDrawn.fetch_add(it.mesh ? it.mesh->indexCount / 3 : 0,
                                 std::memory_order_relaxed);
    };

    // Parentless: the local matrix IS the world matrix. Zero component lookups —
    // Transform, MeshRenderer and the optional PrevTransform all arrive from the
    // query, resolved once per archetype instead of once per entity.
    // ── The parentless path, as a chunk of contiguous component arrays ──────
    // Writes straight into m_items at a reserved offset instead of push_back, so
    // chunks are independent and can run on different threads. Returns how many
    // it wrote, which is <= count when an item's mesh is missing.
    auto extractChunk = [&](ExtractChunk& c) {
        // The one thing that would corrupt memory rather than just render wrong:
        // a chunk writing past its reserved slice. Cheap enough to keep in debug,
        // and this is the invariant every worker thread depends on being disjoint.
        assert(c.outBegin + c.count <= m_items.size());
        RenderItem* out = m_items.data() + c.outBegin;
        // The cull streams are filled HERE, in the same slice, because the model
        // matrix is already in registers — walking the items again later to build
        // them would spend the bandwidth this exists to save.
        CullSphere* sp = m_cull.sphere.data() + c.outBegin;
        uint64_t*           sk = m_cull.keyBase.data() + c.outBegin;

        uint32_t n = 0;
        for (uint32_t i = 0; i < c.count; ++i) {
            RenderItem& it = out[n];
            it = RenderItem{};
            if (!extractItem(c.mr[i], c.skin ? &c.skin[i] : nullptr, it)) continue;
            localMatrixLerp(c.tr[i], c.prev ? &c.prev[i] : nullptr,
                            m_simAlpha, it.model.m);
            rworld::writeCullEntry(it, sp[n], sk[n]);
            if (c.lod) applyLod(c.mr[i], c.lod[i], sp[n], it, sk[n]);
            ++n;
        }
        c.written = n;
    };
    // Parented: walk the chain. Correctness over speed — a child's world matrix
    // genuinely depends on ancestors that this query does not hand us.
    auto extractChild = [&](flecs::entity e, const Transform& tr,
                            const MeshRenderer& mr, const PrevTransform* prev,
                            const SkinnedMesh* skin, const LodMesh* lod) {
        RenderItem it;
        if (!extractItem(mr, skin, it)) return;
        getWorldMatrixLerpFrom(e, tr, prev, m_simAlpha, it.model.m);
        // Stream entry written HERE rather than in a fix-up pass afterwards. It
        // used to be a second loop over the appended tail, which was fine while
        // the entry depended only on the item — LOD selection needs the sphere AND
        // the components, and carrying `mr`/`lod` forward to a later pass would
        // mean side arrays for a handful of items. One append, everything in hand.
        CullSphere sp; uint64_t kb;
        rworld::writeCullEntry(it, sp, kb);
        if (lod) applyLod(mr, *lod, sp, it, kb);
        m_items.push_back(it);
        m_cull.sphere.push_back(sp);
        m_cull.keyBase.push_back(kb);
    };
    auto extractLight = [&](flecs::entity e, const Transform&, const Light& lc) {
        float m[16]; getWorldMatrixLerp(e, m_simAlpha, m);
        LightItem li;
        li.type      = lc.type;
        {
            const bx::Vec3 kc = lc.useTemperature
                ? kelvinToRGB(lc.temperatureK) : bx::Vec3{ 1.0f, 1.0f, 1.0f };
            li.color = bx::Vec3{ kc.x * lc.color.x, kc.y * lc.color.y, kc.z * lc.color.z };
        }
        li.intensity = lc.intensity;
        li.range     = lc.range;
        li.position  = bx::Vec3{ m[12], m[13], m[14] };
        // light emits along local -Z, so toward-light = +local-Z in world
        li.direction = bx::normalize(bx::Vec3{ -m[8], -m[9], -m[10] });
        li.spotInnerCos = std::cos(lc.spotInner * (3.14159265f / 180.0f));
        li.spotOuterCos = std::cos(lc.spotOuter * (3.14159265f / 180.0f));
        li.castShadows  = lc.castShadows;
        m_lights.push_back(li);
    };

    // Slice every matching archetype into job-sized ranges. Serial, and cheap:
    // this walks TABLES, not entities — 20 000 objects sharing one mesh is a
    // single table, hence a few dozen chunks rather than 20 000 iterations.
    auto collectChunks = [&](ItemQuery& q) {
        m_chunks.clear();
        uint32_t total = 0;
        q.run([&](flecs::iter& it) {
            while (it.next()) {
                const uint32_t n = (uint32_t)it.count();
                if (n == 0) continue;
                auto trF = it.field<const Transform>(0);
                auto mrF = it.field<const MeshRenderer>(1);
                const Transform*    tr = &trF[0];
                const MeshRenderer* mr = &mrF[0];
                // Optional terms are absent for a whole ARCHETYPE, never per
                // entity — which is exactly why they cost nothing to read.
                const PrevTransform* prev = it.is_set(2)
                    ? &it.field<const PrevTransform>(2)[0] : nullptr;
                const SkinnedMesh* skin = it.is_set(3)
                    ? &it.field<const SkinnedMesh>(3)[0] : nullptr;
                const LodMesh* lod = it.is_set(4)
                    ? &it.field<const LodMesh>(4)[0] : nullptr;

                for (uint32_t off = 0; off < n; off += kExtractGrain) {
                    const uint32_t c = n - off < kExtractGrain ? n - off
                                                              : kExtractGrain;
                    m_chunks.push_back(ExtractChunk{
                        tr + off, mr + off,
                        prev ? prev + off : nullptr,
                        skin ? skin + off : nullptr,
                        lod ? lod + off : nullptr,
                        c, total, 0 });
                    total += c;
                }
            }
        });
        return total;
    };

    m_items.clear();
    m_lights.clear();
    // Per-VIEW, not per-frame, and that is the honest reading: the census
    // describes the last view extracted. The scene and game views have different
    // cameras and legitimately pick different levels.
    for (auto& c : m_lodCount) c.store(0, std::memory_order_relaxed);
    m_lodBroken.store(0, std::memory_order_relaxed);
    m_lodTrisDrawn.store(0, std::memory_order_relaxed);
    m_lodTrisFull.store(0, std::memory_order_relaxed);

    ItemQuery* flatQ  = nullptr;
    ItemQuery* childQ = nullptr;
    if (&world == m_editorWorld) {              // editor world: cached queries
        flatQ  = &m_itemQuery;
        childQ = &m_childItemQuery;
    } else {                                    // Play snapshot: cached per world
        flatQ = &m_gameItemQuery.get(world, [](auto& b) {
            b.without(flecs::ChildOf, flecs::Wildcard); });
        childQ = &m_gameChildItemQuery.get(world, [](auto& b) {
            b.with(flecs::ChildOf, flecs::Wildcard); });
    }

    // Parentless items: sized up front, then filled in parallel.
    const uint32_t flatTotal = collectChunks(*flatQ);
    if (flatTotal) {
        m_items.resize(flatTotal);
        m_cull.resize(flatTotal);
        const bool parallel = jobs::initialized()
                           && flatTotal >= kExtractParallelMin
                           && m_chunks.size() > 1;
        // ONE body either way. A separate serial implementation would be the
        // version every test exercises and the parallel one the version that
        // ships — the two would drift, and the drift would be invisible.
        if (parallel) {
            jobs::parallelFor("Extract.chunk", (uint32_t)m_chunks.size(), 1,
                [&](uint32_t begin, uint32_t end) {
                    for (uint32_t k = begin; k < end; ++k) extractChunk(m_chunks[k]);
                });
        } else {
            for (auto& c : m_chunks) extractChunk(c);
        }

        // Compact away the gaps left by items whose mesh was missing. Skipped
        // entirely when nothing was dropped, which is every normal frame.
        uint32_t write = m_chunks[0].written;
        for (std::size_t k = 1; k < m_chunks.size(); ++k) {
            const ExtractChunk& c = m_chunks[k];
            if (write != c.outBegin && c.written) {
                // Items AND streams move together, or the streams stop describing
                // the items they are indexed alongside — which the cull would read
                // as the wrong bounds for the wrong object, silently.
                std::memmove(m_items.data() + write, m_items.data() + c.outBegin,
                             c.written * sizeof(RenderItem));
                std::memmove(m_cull.sphere.data() + write,
                             m_cull.sphere.data() + c.outBegin,
                             c.written * sizeof(CullSphere));
                std::memmove(m_cull.keyBase.data() + write,
                             m_cull.keyBase.data() + c.outBegin,
                             c.written * sizeof(uint64_t));
            }
            write += c.written;
        }
        m_items.resize(write);
        m_cull.resize(write);
    }

    // Parented items and lights stay serial: they need an entity handle for the
    // ancestor walk, and they are a small minority of a scene. Appended after
    // the parallel block, so nothing races with the resize above.
    childQ->each(extractChild);
    // extractChild appends its own stream entry, so the two stay parallel by
    // construction. Asserted rather than repaired: a length mismatch means the
    // cull would read one object's bounds for another's, and the failure mode of
    // that is a silently wrong frame, not a crash.
    assert(m_cull.size() == m_items.size());
    if (&world == m_editorWorld) m_lightQuery.each(extractLight);
    else                        m_gameLightQuery.get(world).each(extractLight);

    rv.items   = { m_items.data(),  m_items.size() };
    rv.cull    = m_cull.view();
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}

Renderer::LodCensus Renderer::lodCensus() const {
    LodCensus c;
    for (std::size_t i = 0; i < rworld::kMaxLodLevels; ++i)
        c.level[i] = m_lodCount[i].load(std::memory_order_relaxed);
    c.broken    = m_lodBroken.load(std::memory_order_relaxed);
    c.trisDrawn = m_lodTrisDrawn.load(std::memory_order_relaxed);
    c.trisFull  = m_lodTrisFull.load(std::memory_order_relaxed);
    return c;
}
