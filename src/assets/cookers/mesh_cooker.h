#pragma once
#include <assetlib/cook_pipeline.h>
#include <assetlib/mesh_asset.h>
#include <assimp/matrix3x3.h>
#include <assimp/matrix4x4.h>

class MeshCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 9; // bumped: scale-invariant normal matrix
    std::vector<std::string> extensions() const override {
        return {".fbx",".obj",".dae",".ply",".stl"};
    }
    assetlib::CookResult cook(const assetlib::CookContext& ctx) override;
};

// Normal matrix (inverse-transpose of the linear part) with a SCALE-INVARIANT
// singularity guard. Exposed for tests (cooker audit "Determinant Trap"):
// the old bare |det| > 1e-12 check collapsed for small uniform scales —
// 0.0001^3 IS 1e-12, so valid heavily-scaled-down assets got an identity
// normal matrix and their normals stopped following node rotation (broken
// shading). Falls back to identity only for GENUINELY singular bases.
aiMatrix3x3 cookNormalMatrix(const aiMatrix4x4& world);
