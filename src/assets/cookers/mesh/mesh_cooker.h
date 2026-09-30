#pragma once
#include <assetlib/cooker.h>
#include <assetlib/mesh_asset.h>
#include <filesystem>
#include <string>
#include <vector>

class MeshCooker : public assetlib::ICooker {
public:
    static constexpr uint32_t kVersion = 21; // 21: an animation-only source cooks to a clip
                                             //     (animation/cooked_clip.h), not skipped (WO-016).
                                             // 20: a rig over kMaxBones (128, the GPU palette) is refused,
                                             //     not cooked to draw unanimated (WO-040).
                                             // 19: a skinned mesh is bounded skinned at rest,
                                             //     not in bind space (WO-036).
                                             // 18: every format through ImportedScene and one
                                             // back end (WO-012/013): generated tangents,
                                             // textures as siblings, mirrored instances.
                                             // WO-012 changed glTF output without this bump,
                                             // so glTF cooked in between is re-cooked here.
                                             // 17: v6 blob integrity digests
                                         // 16: LOD levels keep their submesh
                                             // ranges, so a level draws with the
                                             // same materials as its parent
                                             // (15: LOD levels, 16-bit indices too)
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
