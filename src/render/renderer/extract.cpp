// ── Renderer: extraction — ECS world → RenderView ────────────────────────────
//
// ONE concern: turning entities into the flat POD arrays the pipeline consumes.
// This is the only place in the renderer that knows flecs, Transform, or
// MeshRenderer exist; everything downstream sees `RenderItem`/`LightItem` spans.
//
// IT IS ALSO THE HOT PATH, and that is why it is its own translation unit. With
// instancing in place a 20 000-object scene submits ONE draw call and still costs
// ~38 ms a frame — all of it here, per item: a query iteration, a world-matrix
// lerp, three registry lookups, a try_get<SkinnedMesh>, and a push_back. Draw
// submission stopped being the bottleneck; this is what replaced it, so the next
// optimisation (packing extraction output into a pre-sized frame array instead of
// growing vectors, and hoisting the per-item pointer chases) lands in this file
// and nowhere else.
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

#include "core/transform_utils.h"        // getWorldMatrixLerp
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "render/world/frustum.h"        // shared frustum-plane extraction

RenderView Renderer::buildView(flecs::world& world, const float view[16],
                               const float proj[16], const RenderTarget& target,
                               bgfx::ViewId baseViewId) {
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

    auto extractItem = [&](flecs::entity e, const Transform&, const MeshRenderer& mr) {
        const Mesh* mesh = m_assets->getMesh(mr.mesh);
        if (!mesh) return;
        RenderItem it;
        getWorldMatrixLerp(e, m_simAlpha, it.model.m);
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
        m_itemQuery.each(extractItem);
        m_lightQuery.each(extractLight);
    } else {                                    // Play snapshot: cached per world
        m_gameItemQuery.get(world).each(extractItem);
        m_gameLightQuery.get(world).each(extractLight);
    }

    rv.items   = { m_items.data(),  m_items.size() };
    rv.lights  = { m_lights.data(), m_lights.size() };
    rv.ambient = 0.25f;
    return rv;
}
