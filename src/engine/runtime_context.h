#pragma once
#include <flecs.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "io/project_context.h"
#include "io/importer_registry.h"
#include <assetlib/asset_registry.h>

class PrimitiveLibrary; // forward declare

struct RuntimeContext {
    flecs::world&     ecs;
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
    ProjectContext&   project;
    ImporterRegistry& importers;
    assetlib::AssetRegistry* assetLib   = nullptr;
    PrimitiveLibrary*        primitives = nullptr;
};
