// ── Renderer: extraction — ECS world → RenderView ────────────────────────────
//
// ONE concern: turning entities into the flat POD arrays the pipeline consumes.
// This is the only place in the renderer that knows flecs, Transform, or
// MeshRenderer exist; everything downstream sees `RenderItem`/`LightItem` spans.
//
// IT IS ALSO THE HOT PATH, and that is why it is its own translation unit. Draw
// submission stopped being the bottleneck once instancing landed — a 20 000-object
// scene submits ONE draw call — and this is what replaced it. MEASURED, with the
// Render.extract profiler zone, at 20 000 objects:
//
//   before   15.2 ms extract, of a 27.1 ms frame
//   after     9.7 ms extract, of a 17.8 ms frame   (cadence p50 17.75, 54.9 fps)
//
// (The pre-fix cadence was not captured, only the 27.1 ms frame cost, so the fps
// figure is quoted for the after state alone rather than as a before/after pair.)
//
// The 5.5 ms came from ONE realisation, and it is the thing to remember before
// optimising anything here: almost none of the cost was arithmetic. It was
// per-entity component lookups asking questions the query engine answers per
// ARCHETYPE — try_get<PrevTransform>, a redundant try_get<Transform>, and
// target(ChildOf) + is_alive() + has<Transform>() to learn "no parent" 20 000
// times. The fix was in the query declarations (renderer.h), not in this loop.
//
// Measured NOT to matter, so nobody spends a day on them again: the three
// registry lookups are ~0.17 ms total, and reserving m_items up front is ~0.4 ms.
// What is left is genuine work — the interpolation and matrix compose — so the
// next real win is width (SIMD / jobs over archetype spans), not lookup removal.
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

#include <cmath>

#include <bx/math.h>

#include "core/profiler.h"
#include "core/transform_utils.h"        // getWorldMatrixLerp
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "render/world/frustum.h"        // shared frustum-plane extraction

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
    auto extractItem = [&](flecs::entity e, const MeshRenderer& mr,
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

        // Skinned mesh: pass the bone palette to the pipeline
        const SkinnedMesh* skin = e.try_get<SkinnedMesh>();
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
    auto extractFlat = [&](flecs::entity e, const Transform& tr,
                           const MeshRenderer& mr, const PrevTransform* prev) {
        RenderItem it;
        if (!extractItem(e, mr, it)) return;
        localMatrixLerp(tr, prev, m_simAlpha, it.model.m);
        m_items.push_back(it);
    };
    // Parented: walk the chain. Correctness over speed — a child's world matrix
    // genuinely depends on ancestors that this query does not hand us.
    auto extractChild = [&](flecs::entity e, const Transform& tr,
                            const MeshRenderer& mr, const PrevTransform* prev) {
        RenderItem it;
        if (!extractItem(e, mr, it)) return;
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

    m_items.clear();
    m_lights.clear();
    if (&world == m_editorWorld) {              // editor world: cached queries
        m_itemQuery.each(extractFlat);
        m_childItemQuery.each(extractChild);
        m_lightQuery.each(extractLight);
    } else {                                    // Play snapshot: cached per world
        m_gameItemQuery.get(world, [](auto& b) {
            b.without(flecs::ChildOf, flecs::Wildcard); }).each(extractFlat);
        m_gameChildItemQuery.get(world, [](auto& b) {
            b.with(flecs::ChildOf, flecs::Wildcard); }).each(extractChild);
        m_gameLightQuery.get(world).each(extractLight);
    }

    rv.items   = { m_items.data(),  m_items.size() };
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}
