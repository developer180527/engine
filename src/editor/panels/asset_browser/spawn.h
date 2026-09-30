#pragma once
#include "types.h"
#include "editor/engine_context.h"
#include "runtime/services/async_loader.h"
#include "core/logger.h"
#include "assets/asset_storage.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/animator.h"
#include "render/mesh.h"
#include "render/primitive_library.h"
#include "assets/asset_ref.h"
#include "components/mesh_placeholder.h"
#include "scene/unresolved_mesh.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <flecs.h>
#include <string>

// Returns a unique entity name, appending (2), (3)... on collision.
inline std::string uniqueEntityName(flecs::world& ecs, const std::string& base) {
    if (!ecs.lookup(base.c_str())) return base;
    for (int i = 2; i < 10000; ++i) {
        std::string c = base + " (" + std::to_string(i) + ")";
        if (!ecs.lookup(c.c_str())) return c;
    }
    return base;
}

namespace ab {

// Scale mesh to ~2 world units using its AABB.
inline float autoScale(const Mesh* m) {
    if (!m || !m->hasBounds()) return 1.0f;
    auto sz = m->boundsSize();
    float big = std::max({sz.x, sz.y, sz.z});
    return big < 1e-4f ? 1.0f : 2.0f / big;
}

// Y offset to place mesh flush with Y=0 after scaling.
inline float groundOffset(const Mesh* m, float sc) {
    return (!m || !m->hasBounds()) ? 0.0f : -m->boundsMin.y * sc;
}

// Where a spawned entity starts: an identity transform, so the load callback
// can tell whether anyone has moved it since.
inline const Transform kSpawnTransform{};

// Spawn a model into the scene, from its COOKED version (WO-018).
//
// The entity exists at once. If the model is cooked it fills in within a
// frame or two; if not, the loader asks the editor's CookService for it and
// the entity shows the placeholder cube until the cook lands, then swaps in.
// A failed cook leaves the placeholder, with the reason in the Inspector.
//
// The entity carries its AUTHORED reference (UnresolvedMesh) the whole time,
// so saving the scene mid-cook writes the model, not the cube. There used to
// be a second route for uncooked glTF (the runtime cgltf importer, static
// geometry only) and a third for everything else (Assimp in the loader); both
// parsed the source in-process, so a model looked different until it cooked.
inline void spawnFile(const FileEntry& f, EngineContext& ctx, AsyncLoader& loader) {
    auto& ecs = ctx.ecs;
    const std::string en = uniqueEntityName(ecs, baseName(f.name));

    nlohmann::json authored;
    assetref::toJson(assetref::make(f.fullPath, ctx.project.projectRoot, ctx.assetLib), authored);
    flecs::entity ent = ecs.entity(en.c_str())
        .set<Transform>(Transform{}).set<Name>({en})
        .set<UnresolvedMesh>({authored.dump(), "loading", true});
    if (ctx.primitives && ctx.primitives->ready()) {
        const MeshHandle cube = ctx.primitives->cube();
        ent.set<MeshRenderer>({cube}).set<MeshPlaceholder>({cube});
    }
    ctx.editor.selected = ent;

    auto& assets = ctx.assets;
    const flecs::entity_t id = ent.id();
    loader.load(f.fullPath, en,
        [&ecs, &assets, id, en](const AsyncLoadResult& r, const std::string&) {
            flecs::entity e = ecs.entity(id);
            if (e.id() == 0 || !e.is_alive()) return;   // deleted while cooking
            if (!r.mesh.valid()) {
                if (UnresolvedMesh* u = e.try_get_mut<UnresolvedMesh>()) {
                    u->reason = r.error.empty() ? "load failed" : r.error;
                    u->pending = false;
                }
                LOG_ERROR("Loader", "'%s': %s — placeholder kept", en.c_str(), r.error.c_str());
                return;
            }
            const Mesh* mesh = assets.getMesh(r.mesh);
            // Fit it to ~2 units on the ground, unless someone moved it while
            // it cooked: their placement wins.
            if (const Transform* cur = e.try_get<Transform>();
                cur && std::memcmp(cur, &kSpawnTransform, sizeof(Transform)) == 0) {
                const float sc = autoScale(mesh), yo = groundOffset(mesh, sc);
                Transform t; t.position = {0,yo,0}; t.scale = {sc,sc,sc};
                e.set<Transform>(t);
            }
            e.set<MeshRenderer>({r.mesh});
            e.remove<UnresolvedMesh>();
            e.remove<MeshPlaceholder>();

            // Attach skeletal animation components when bones are present
            if (r.skeleton.valid()) {
                SkinnedMesh sm;
                sm.skeleton = r.skeleton;
                e.set<SkinnedMesh>(sm);
                Animator anim;
                if (!r.clips.empty()) {
                    anim.clip    = r.clips[0];
                    anim.playing = true;  // auto-play first clip on import
                }
                e.set<Animator>(anim);
            }
            LOG_SUCCESS("Loader", "Spawned '%s'%s", en.c_str(),
                        r.skeleton.valid() ? " [skinned]" : "");
        });
}

} // namespace ab
