#pragma once
#include <assetlib/cooker.h>
#include <assetlib/mesh_asset.h>
#include <assimp/matrix3x3.h>
#include <assimp/matrix4x4.h>

class MeshCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 13; // 13: sibling .ctex content dedup
                                             // (materials sharing an image now
                                             // write ONE file, not one each).
                                             // 10: glTF/GLB cook (cgltf).
                                             // 11: DDC transition — embedded
                                             // .ctex outputs reported via
                                             // ctx.addOutput.
                                             // 12: rgbcx/bc7enc texture
                                             // encoders (.ctex bytes change).
    std::vector<std::string> extensions() const override {
        return {".fbx",".obj",".dae",".ply",".stl",".gltf",".glb"};
    }
    assetlib::CookResult cook(const assetlib::CookContext& ctx) override;

    const char* id()      const override { return "mesh"; }
    uint32_t    version() const override { return kVersion; }
    // Embedded/material textures run through the shared encode path, so the
    // COOK_TEX_HQ tier changes THIS cooker's output too.
    std::string settingsFingerprint(const assetlib::CookContext&) const override;

    // Sibling .ctex files of an already-cooked mesh, read back from its own
    // material table (the exact set cook() reported via addOutput) so the DDC
    // can be back-filled without re-cooking. Never globs — see
    // ICooker::enumerateOutputs.
    void enumerateOutputs(const std::filesystem::path& primary,
                          std::vector<std::filesystem::path>& out) const override;
};

// Normal matrix (inverse-transpose of the linear part) with a SCALE-INVARIANT
// singularity guard. Exposed for tests (cooker audit "Determinant Trap"):
// the old bare |det| > 1e-12 check collapsed for small uniform scales —
// 0.0001^3 IS 1e-12, so valid heavily-scaled-down assets got an identity
// normal matrix and their normals stopped following node rotation (broken
// shading). Falls back to identity only for GENUINELY singular bases.
aiMatrix3x3 cookNormalMatrix(const aiMatrix4x4& world);
