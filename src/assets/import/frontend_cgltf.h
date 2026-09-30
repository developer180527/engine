#pragma once
// ── CgltfFrontend — glTF 2.0 (.gltf, .glb) -> ImportedScene (WO-012) ─────────
//
// The first real import front end. It converts; it decides nothing about
// cooking. Baking, tangent generation, texture encoding and LODs are the back
// end's (assets/cookers/mesh/mesh_backend.cpp). What it cannot carry goes in
// ImportedScene::dropped, with an effect: Wrong (the cook is refused) or Less.
//
// glTF is already in the engine's conventions (right-handed, +Y up, metres,
// top-left UVs, CCW front faces), so nothing is converted, only read.
#include "assets/import/import_frontend.h"

namespace imp {

class CgltfFrontend final : public IImportFrontend {
public:
    const char* name() const override { return "cgltf"; }
    std::vector<std::string> extensions() const override { return {"gltf", "glb"}; }
    ImportResult importScene(const std::filesystem::path& source,
                             const ImportOptions& options) const override;
};

}  // namespace imp
