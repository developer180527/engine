#pragma once
#include <flecs.h>
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "io/project_context.h"
#include "io/importer_registry.h"

struct RuntimeContext {
    flecs::world&     ecs;
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
    ProjectContext&   project;
    ImporterRegistry& importers;
};
