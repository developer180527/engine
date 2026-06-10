#pragma once
#include <flecs.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "project/project_context.h"
#include "assets/importers/importer_registry.h"
#include <assetlib/asset_registry.h>

class PrimitiveLibrary;   // forward declare
class AssetService;       // forward declare
class SceneService;       // forward declare
class SkeletonRegistry;   // forward declare
class AnimClipRegistry;   // forward declare
class ScriptHost;         // forward declare

struct RuntimeContext {
    flecs::world&     ecs;
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
    ProjectContext&   project;
    ImporterRegistry& importers;
    assetlib::AssetRegistry* assetLib     = nullptr;
    PrimitiveLibrary*        primitives   = nullptr;
    AssetService*            assetService = nullptr;
    SceneService*            sceneService = nullptr;
    SkeletonRegistry*        skeletons    = nullptr;
    AnimClipRegistry*        clips        = nullptr;
    // The canonical scripting surface — runtime-owned; Lua bindings, the
    // C API (engine_api.h) and future language hosts all drive this one.
    ScriptHost*              scriptHost   = nullptr;
};
