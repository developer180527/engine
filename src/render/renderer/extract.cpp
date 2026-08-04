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
//     5 000   0.50      0.55    8.3 (vsync)
//    20 000   1.13      1.70    7.9 (vsync)
//    50 000   2.56      3.91    9.3   <- cull > extract now
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
// was NOT done and is not the next lever. The per-item maths that remains is spread
// across cores, and within the render path CULL is now larger than extract (3.9 ms
// vs 2.6 at 50 000 objects) — that is where the next work goes. Sim.prevSnapshot,
// which used to be ~25 ms of a 34 ms frame at that size, is fixed by the same
// per-archetype reasoning (src/runtime/docs/issues.md H.0), so the frame is 9.3 ms
// and the renderer is 8.4 of it.
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

#include <bx/math.h>

#include "core/profiler.h"
#include "runtime/jobs/jobs.h"
#include "core/transform_utils.h"        // getWorldMatrixLerp
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "render/world/frustum.h"        // shared frustum-plane extraction

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
        it.mat = mh.valid() ? m_materials->getMaterial(mh) : nullptr;
        it.tex = (it.mat && it.mat->hasTexture())
                 ? m_textures->getTexture(it.mat->baseColorTexture) : nullptr;
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
        uint32_t n = 0;
        for (uint32_t i = 0; i < c.count; ++i) {
            RenderItem& it = out[n];
            it = RenderItem{};
            if (!extractItem(c.mr[i], c.skin ? &c.skin[i] : nullptr, it)) continue;
            localMatrixLerp(c.tr[i], c.prev ? &c.prev[i] : nullptr,
                            m_simAlpha, it.model.m);
            ++n;
        }
        c.written = n;
    };
    // Parented: walk the chain. Correctness over speed — a child's world matrix
    // genuinely depends on ancestors that this query does not hand us.
    auto extractChild = [&](flecs::entity e, const Transform& tr,
                            const MeshRenderer& mr, const PrevTransform* prev,
                            const SkinnedMesh* skin) {
        RenderItem it;
        if (!extractItem(mr, skin, it)) return;
        getWorldMatrixLerpFrom(e, tr, prev, m_simAlpha, it.model.m);
        m_items.push_back(it);
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

                for (uint32_t off = 0; off < n; off += kExtractGrain) {
                    const uint32_t c = n - off < kExtractGrain ? n - off
                                                              : kExtractGrain;
                    m_chunks.push_back(ExtractChunk{
                        tr + off, mr + off,
                        prev ? prev + off : nullptr,
                        skin ? skin + off : nullptr,
                        c, total, 0 });
                    total += c;
                }
            }
        });
        return total;
    };

    m_items.clear();
    m_lights.clear();

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
        bool gaps = false;
        for (std::size_t k = 1; k < m_chunks.size(); ++k) {
            const ExtractChunk& c = m_chunks[k];
            if (write != c.outBegin) {
                gaps = true;
                std::memmove(m_items.data() + write, m_items.data() + c.outBegin,
                             c.written * sizeof(RenderItem));
            }
            write += c.written;
        }
        (void)gaps;
        m_items.resize(write);
    }

    // Parented items and lights stay serial: they need an entity handle for the
    // ancestor walk, and they are a small minority of a scene. Appended after
    // the parallel block, so nothing races with the resize above.
    childQ->each(extractChild);
    if (&world == m_editorWorld) m_lightQuery.each(extractLight);
    else                        m_gameLightQuery.get(world).each(extractLight);

    rv.items   = { m_items.data(),  m_items.size() };
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}
