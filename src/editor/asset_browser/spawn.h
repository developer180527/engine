#pragma once
#include "types.h"
#include "engine_context.h"
#include "engine/async_loader.h"
#include "engine/logger.h"
#include "io/asset_storage.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/animator.h"
#include "render/mesh.h"
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

// Spawn a file into the scene — routes by extension.
// glTF/GLB: synchronous via cgltf.
// Everything else: async via AsyncLoader + Assimp/binary fallback.
inline void spawnFile(const FileEntry& f, EngineContext& ctx, AsyncLoader& loader) {
    const bool isGltf = (f.ext == ".glb" || f.ext == ".gltf");

    if (isGltf) {
        AssetStorage s{ctx.assets, ctx.textures, ctx.materials,
                       ctx.skeletons, ctx.clips};
        auto r = ctx.importers.loadCached(f.fullPath, s);
        if (r.success) {
            if (Mesh* m = const_cast<Mesh*>(ctx.assets.getMesh(r.mesh)))
                m->sourcePath = f.fullPath;
            const Mesh* mesh = ctx.assets.getMesh(r.mesh);
            float sc = autoScale(mesh), yo = groundOffset(mesh, sc);
            Transform t; t.position = {0,yo,0}; t.scale = {sc,sc,sc};
            std::string en = uniqueEntityName(ctx.ecs, baseName(f.name));
            auto ent = ctx.ecs.entity(en.c_str())
                .set<Transform>(t).set<MeshRenderer>({r.mesh}).set<Name>({en});

            // Attach skeletal animation components when bones are present
            if (r.skeleton.valid()) {
                SkinnedMesh sm;
                sm.skeleton = r.skeleton;
                ent.set<SkinnedMesh>(sm);
                Animator anim;
                if (!r.clips.empty()) anim.clip = r.clips[0];
                ent.set<Animator>(anim);
            }

            ctx.editor.selected = ent;
            LOG_SUCCESS("Loader", "Spawned '%s' (glTF)%s", en.c_str(),
                        r.skeleton.valid() ? " [skinned]" : "");
        } else {
            LOG_ERROR("Loader", "glTF load failed: %s", r.error.c_str());
        }
    } else {
        auto& ecs = ctx.ecs; auto& assets = ctx.assets; auto& ed = ctx.editor;
        std::string en = uniqueEntityName(ctx.ecs, baseName(f.name));
        loader.load(f.fullPath, en,
            [&ecs, &assets, &ed, en](MeshHandle h, const std::string&) {
                if (!h.valid()) return;
                const Mesh* mesh = assets.getMesh(h);
                float sc = autoScale(mesh), yo = groundOffset(mesh, sc);
                Transform t; t.position = {0,yo,0}; t.scale = {sc,sc,sc};
                ed.selected = ecs.entity(en.c_str())
                    .set<Transform>(t).set<MeshRenderer>({h}).set<Name>({en});
                LOG_SUCCESS("Loader", "Spawned '%s'", en.c_str());
            });
    }
}

} // namespace ab
