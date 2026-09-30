#pragma once
// ── AssimpFrontend — FBX, OBJ, COLLADA, 3DS, PLY, STL, Blend -> ImportedScene ──
//
// WO-013. The second real import front end, and the last place Assimp's types
// exist in the cook stack: aiScene never leaves this file (audit IMP-01). It
// converts; baking, tangents, textures, LODs, the ozz skeleton and clips are the
// back end's (assets/cookers/mesh/mesh_backend.cpp).
//
// It reproduces what the old Assimp cook paths did BEFORE cooking, so their
// output carries over: the same post-processing flags, FBX pivots baked
// (PRESERVE_PIVOTS=false, which the skinned path always used), nodes in
// depth-first pre-order, the skeleton and weights from the same extractors, and
// Mixamo's junk clip names replaced by the file's stem.
//
// UNITS ARE NOT CONVERTED. Assimp's FBX reader applies UnitScaleFactor in
// centimetres, so an FBX arrives in the units it was authored in, as it always
// has. Changing that would rescale every FBX in every project; it is its own
// decision (WO-035), not part of moving the parser.
#include "assets/import/import_frontend.h"

namespace imp {

class AssimpFrontend final : public IImportFrontend {
public:
    const char* name() const override { return "assimp"; }
    std::vector<std::string> extensions() const override {
        return {"fbx", "obj", "dae", "3ds", "ply", "stl", "blend"};
    }
    ImportResult importScene(const std::filesystem::path& source,
                             const ImportOptions& options) const override;
};

}  // namespace imp
