#pragma once
#include <assetlib/cook_pipeline.h>
#include <assetlib/mesh_asset.h>

class MeshCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 7; // bumped: material section in cooked file // bumped: fixed multi-submesh vertex rebasing
    std::vector<std::string> extensions() const override {
        return {".fbx",".obj",".dae",".ply",".stl"};
    }
    assetlib::CookResult cook(const assetlib::CookContext& ctx) override;
};
