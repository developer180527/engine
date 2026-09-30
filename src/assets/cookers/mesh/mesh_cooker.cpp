// ── MeshCooker — every mesh format, through the import pipeline ──────────────
//
// A source file is read by the front end that owns its format and becomes an
// imp::ImportedScene; meshcook::cookImportedScene (mesh_backend.cpp) turns that
// into the cooked mesh and its sibling textures. This file only dispatches.
//
//   .gltf .glb                     imp::CgltfFrontend   (WO-012)
//   .fbx .obj .dae .ply .stl ...   imp::AssimpFrontend  (WO-013)
//
// Until WO-012/013 each format had its own complete cook path here, three of
// them, disagreeing on textures, tangents and skinning (imported-scene.md §7.1).
// Nothing in this file names a parser's types any more (audit IMP-01).
//
// MeshCooker::cook is a pure function: the front end owns its parser state and
// the back end writes one unique output, so the cook pipeline runs many cooks
// concurrently.
#include "assets/cookers/mesh/mesh_cooker.h"
#include "assets/cookers/clip/clip_cook.h"
#include "assets/cookers/mesh/mesh_backend.h"
#include "assets/import/frontend_assimp.h"
#include "assets/import/frontend_cgltf.h"

#include <assetlib/texture_asset.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

using namespace assetlib;

std::string MeshCooker::settingsFingerprint(const CookContext&) const {
    // Embedded/material textures share cook::encodeTexture, so the quality
    // tier changes this cooker's .ctex outputs (mirrors texture_encode.cpp).
    const char* hqEnv = std::getenv("COOK_TEX_HQ");
    const bool  hq    = hqEnv && *hqEnv && hqEnv[0] != '0';
    // `ctex=`: this cooker writes sibling .ctex files, so the cooked-texture
    // format version keys it too — see TextureCooker::settingsFingerprint. Without
    // it, a v3 bump left every embedded texture as a cached v2 (legacy) sibling.
    return std::string("hq=") + (hq ? "1" : "0")
         + ";ctex=" + std::to_string(assetlib::TextureHeader{}.version);
}

void MeshCooker::enumerateOutputs(const std::filesystem::path& primary,
                                  std::vector<std::filesystem::path>& out) const {
    // Read the sibling set back out of the cooked mesh itself. cook() writes
    // each embedded texture as "<uuid>_tN.ctex" and stores that BASENAME in the
    // material record, so the material table is an exact list of what this cook
    // produced — unlike a glob, which would also match stale siblings that an
    // earlier cooker version left behind (nothing prunes .cache).
    assetlib::MeshAsset mesh;
    if (!assetlib::loadMesh(mesh, primary)) return;

    const auto dir = primary.parent_path();
    auto add = [&](const char* name, uint32_t flag, uint32_t flags) {
        if (!(flags & flag) || name[0] == '\0') return;
        auto p = dir / name;
        if (std::find(out.begin(), out.end(), p) == out.end())
            out.push_back(std::move(p));   // materials commonly share a texture
    };
    for (const auto& m : mesh.materials) {
        add(m.baseColorPath, assetlib::kMatFlag_HasBaseColor, m.flags);
        add(m.normalMapPath, assetlib::kMatFlag_HasNormalMap, m.flags);
    }
}

CookResult MeshCooker::cook(const CookContext& ctx) {
    std::string ext = ctx.sourcePath.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

    // glTF is cgltf's (Assimp is built without it); everything else is Assimp's.
    const bool gltf = ext == ".gltf" || ext == ".glb";
    imp::ImportResult r = gltf ? imp::CgltfFrontend().importScene(ctx.sourcePath, {})
                               : imp::AssimpFrontend().importScene(ctx.sourcePath, {});
    if (!r) return {.success = false, .error = std::string(gltf ? "glTF: " : "") + r.error().message};
    // Clips and no triangles: an animation file (a Mixamo clip FBX). It cooks
    // as a clip, not a mesh (WO-016; it used to be skipped and cooked only when
    // the editor first played it).
    size_t triangles = 0;
    for (const auto& m : r.scene().meshes) triangles += m.indices.size() / 3;
    if (triangles == 0 && !r.scene().clips.empty()) return clipcook::cookClip(r.scene(), ctx);
    return meshcook::cookImportedScene(r.scene(), ctx);
}
