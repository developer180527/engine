#pragma once
// ── scene_assets_host — the runtime's SceneAssets (WO-047) ────────────────────
//
// src/scene describes what a scene needs through SceneAssets and never includes
// the runtime; this is where the runtime answers, from its own services. Either
// service may be null (a tool with no async loader, a test): the matching hooks
// are left empty and the serializers take their fallbacks.
#include "runtime/services/asset_service.h"
#include "runtime/services/async_loader.h"
#include "scene/scene_assets.h"

inline SceneAssets sceneAssetsFor(AssetService* service, AsyncLoader* loader) {
    SceneAssets a;
    if (service) {
        a.loadCookedMesh = [service](const char* path, std::vector<MeshHandle>* lods) {
            AssetService::MeshLods l;
            const MeshHandle h = service->loadMesh(path, nullptr, lods ? &l : nullptr);
            if (lods) *lods = std::move(l.levels);
            return h;
        };
        a.loadMaterial = [service](const char* name) { return service->loadMaterialAsset(name); };
        a.materialName = [service](MaterialHandle h) { return service->materialNameOf(h); };
    }
    if (loader)
        a.streamMesh = [loader](const std::string& path, const std::string& label,
                                std::function<void(const StreamedMesh&)> done) {
            loader->load(path, label, [done = std::move(done)](const AsyncLoadResult& r, const std::string&) {
                done({r.mesh, r.skeleton, r.clips, r.error});
            });
        };
    return a;
}
