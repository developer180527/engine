#pragma once
#include "mesh_importer.h"

class GltfImporter : public MeshImporter {
public:
    bool             supports(std::string_view extension) const override;
    MeshImportResult load(const std::string& path,
                          AssetStorage& storage) override;
};
