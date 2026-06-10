#pragma once
#include "assets/importers/mesh_importer.h"

// Handles FBX, OBJ, DAE, 3DS, PLY, STL, BLEND and more.
// glTF/GLB are intentionally excluded — cgltf handles those faster
// and with better spec compliance.
class AssimpImporter : public MeshImporter {
public:
    bool             supports(std::string_view ext) const override;
    MeshImportResult load(const std::string& path, AssetStorage& storage) override;
};
