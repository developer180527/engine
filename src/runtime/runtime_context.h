#pragma once
#include <cstdint>
#include <flecs.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "project/project_context.h"
#include <assetlib/asset_registry.h>

class PrimitiveLibrary;   // forward declare
class AssetService;       // forward declare
class SceneService;       // forward declare
class SkeletonRegistry;   // forward declare
class AnimClipRegistry;   // forward declare
class ClipLibrary;        // forward declare
class ScriptHost;         // forward declare

struct RuntimeContext {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    flecs::world&     ecs;
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
    ProjectContext&   project;
    assetlib::AssetRegistry* assetLib     = nullptr;
    PrimitiveLibrary*        primitives   = nullptr;
    AssetService*            assetService = nullptr;
    SceneService*            sceneService = nullptr;
    SkeletonRegistry*        skeletons    = nullptr;
    AnimClipRegistry*        clips        = nullptr;
    // Standalone-clip loader + bind cache (animation/clip_library.h).
    ClipLibrary*             clipLibrary  = nullptr;
    // The canonical scripting surface — runtime-owned; Lua bindings, the
    // C API (engine_api.h) and future language hosts all drive this one.
    ScriptHost*              scriptHost   = nullptr;
};
