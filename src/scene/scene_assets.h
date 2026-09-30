#pragma once
// ── scene_assets — how loading a scene reaches assets, without the runtime ────
//
// A scene file names assets: a cooked mesh path, an authored material name, or
// a source model to import. Resolving those needs the runtime's services
// (AssetService, the async loader), and the serializers used to include them
// directly, which made src/scene and src/runtime depend on each other: the
// last back-edge of the module cycle (WO-047). The runtime already uses scene
// to load and snapshot worlds, so this is the direction that has to go.
//
// Now the serializers DESCRIBE what they need through these hooks and the host
// supplies them (runtime/services/scene_assets_host.h builds them from its own
// services). A test can supply fakes. Every hook is optional: an empty one
// means "this host cannot do that", and the serializer takes its fallback.
//
// Cost: the hooks run while a scene LOADS or SAVES, once per asset reference,
// wrapping work that costs milliseconds (a cooked-mesh load, a source import).
// No tick calls them; sim_profile's 600-tick runs show no serializer scope.
#include <functional>
#include <string>
#include <vector>

#include "core/handle.h"

// What streaming a source mesh produced: the handles a spawned entity is wired
// with. Invalid mesh on failure.
struct StreamedMesh {
    MeshHandle                  mesh;         // invalid: failed, see `error`
    SkeletonHandle              skeleton;     // invalid if not skinned
    std::vector<AnimClipHandle> clips;        // empty if no animations
    std::string                 error;        // why, when `mesh` is invalid
};

struct SceneAssets {
    // A cooked mesh (path relative to the project's .cache), and its coarser
    // LOD levels into `lods` when non-null. Invalid handle on failure.
    std::function<MeshHandle(const char* cookedPath, std::vector<MeshHandle>* lods)> loadCookedMesh;

    // An authored material by NAME (what a scene file stores). Invalid if none.
    std::function<MaterialHandle(const char* name)> loadMaterial;

    // Save: a material handle back to its authored name; empty if it has none.
    std::function<std::string(MaterialHandle)> materialName;

    // Load a model by its SOURCE path, from its cooked version, off the
    // calling thread; `done` runs once, on the thread that drains the loader,
    // with the mesh or the reason there is none. An uncooked source is a cook
    // job where the host can cook, and a failure where it cannot (WO-018).
    std::function<void(const std::string& sourcePath, const std::string& label,
                       std::function<void(const StreamedMesh&)> done)> streamMesh;
};
