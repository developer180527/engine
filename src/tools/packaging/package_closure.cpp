#include "tools/packaging/package_closure.h"

#include <assetlib/mesh_asset.h>
#include <assetlib/scene_asset.h>
#include <assetlib/material_asset.h>
#include <assetlib/shader_asset.h>

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

namespace pkg {

MeshClosure meshClosure(const fs::path& cookedMesh) {
    MeshClosure out;
    std::error_code ec;

    if (!fs::exists(cookedMesh, ec)) { out.unreadable = true; return out; }

    assetlib::MeshAsset ma;
    if (!assetlib::loadMesh(ma, cookedMesh)) { out.unreadable = true; return out; }

    out.files.push_back(cookedMesh);

    auto already = [&](const fs::path& p) {
        return std::find(out.files.begin(), out.files.end(), p) != out.files.end();
    };

    for (const auto& mat : ma.materials) {
        for (const char* ref : { mat.baseColorPath, mat.normalMapPath }) {
            if (!ref || !*ref) continue;
            // Stored as a basename by the cooker; take .filename() anyway so a
            // path-shaped reference cannot escape the cooked directory.
            const fs::path src = cookedMesh.parent_path() / fs::path(ref).filename();
            if (!fs::exists(src, ec)) {
                if (std::find(out.missing.begin(), out.missing.end(), ref)
                    == out.missing.end())
                    out.missing.emplace_back(ref);
                continue;
            }
            if (!already(src)) out.files.push_back(src);
        }
    }
    return out;
}

SceneRefs sceneMeshRefs(const fs::path& scenesDir) {
    SceneRefs out;
    std::error_code ec;
    if (!fs::exists(scenesDir, ec)) return out;

    // Sorted, so the shipped set is byte-identical across machines and reruns.
    // directory_iterator order is filesystem-defined; a package that differs
    // run to run makes "did this build change?" unanswerable.
    std::vector<fs::path> scenes;
    for (const auto& e : fs::directory_iterator(scenesDir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".cooked")
            scenes.push_back(e.path());
    std::sort(scenes.begin(), scenes.end());

    std::set<std::string> seen;
    for (const auto& path : scenes) {
        assetlib::SceneAsset scene;
        if (!assetlib::loadScene(scene, path)) {
            out.unreadableScenes.push_back(path.filename().string());
            continue;
        }
        ++out.scenesRead;
        for (const auto& ent : scene.entities) {
            std::string p = assetlib::stringTableRead(
                scene.stringTable, ent.meshCookedOffset, ent.meshCookedLength);
            if (p.empty()) continue;                 // entity has no mesh
            if (seen.insert(p).second) out.meshes.push_back(std::move(p));
        }
    }
    return out;
}

bool ShaderSet::provides(const std::string& name) const {
    return std::find(names.begin(), names.end(), name) != names.end();
}

ShaderSet shaderFiles(const fs::path& shadersDir) {
    ShaderSet out;
    std::error_code ec;
    if (!fs::exists(shadersDir, ec)) return out;

    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(shadersDir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".cooked")
            found.push_back(e.path());
    std::sort(found.begin(), found.end());   // deterministic package

    std::set<std::string> seenNames;
    for (const auto& path : found) {
        assetlib::ShaderAsset sh;
        if (!assetlib::loadShader(sh, path) || sh.name.empty()) {
            out.unreadable.push_back(path.filename().string());
            continue;
        }
        // Still shipped even when the name duplicates: dropping one arbitrarily
        // would make the package depend on which file the packager saw first,
        // and ShaderLibrary already resolves the ambiguity deterministically.
        out.files.push_back(path);
        if (!seenNames.insert(sh.name).second) {
            if (std::find(out.duplicateNames.begin(), out.duplicateNames.end(),
                          sh.name) == out.duplicateNames.end())
                out.duplicateNames.push_back(sh.name);
        } else {
            out.names.push_back(sh.name);
        }
    }
    std::sort(out.names.begin(), out.names.end());
    return out;
}

bool MaterialSet::provides(const std::string& name) const {
    return std::find(names.begin(), names.end(), name) != names.end();
}

MaterialSet materialFiles(const fs::path& materialsDir) {
    MaterialSet out;
    std::error_code ec;
    if (!fs::exists(materialsDir, ec)) return out;

    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(materialsDir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".cooked")
            found.push_back(e.path());
    std::sort(found.begin(), found.end());

    std::set<std::string> seen;
    for (const auto& path : found) {
        assetlib::MaterialAsset ma;
        if (!assetlib::loadMaterial(ma, path) || ma.name.empty()) {
            out.unreadable.push_back(path.filename().string());
            continue;
        }
        out.files.push_back(path);
        if (!seen.insert(ma.name).second) {
            if (std::find(out.duplicateNames.begin(), out.duplicateNames.end(),
                          ma.name) == out.duplicateNames.end())
                out.duplicateNames.push_back(ma.name);
        } else {
            out.names.push_back(ma.name);
        }
    }
    std::sort(out.names.begin(), out.names.end());
    return out;
}

// ── Material textures, resolved for a runtime with no registry ──────────────
MaterialTextureSet resolveMaterialTextures(const MaterialSet& materials,
                                           assetlib::AssetRegistry& registry,
                                           const fs::path& cacheRoot,
                                           const fs::path& outDir) {
    MaterialTextureSet out;
    std::set<std::string> shipped;
    std::set<std::string> missing;

    for (const fs::path& file : materials.files) {
        assetlib::MaterialAsset ma;
        if (!assetlib::loadMaterial(ma, file)) continue;   // already reported

        for (auto& t : ma.textures) {
            if (t.path.empty()) continue;          // fallback-only slot, fine

            // Source paths are stored the way an author typed them, relative to
            // the project root — which is exactly the key the registry indexes.
            auto rec = registry.findBySourcePath(t.path);
            if (!rec || rec->cookedPath.empty()
                || rec->state != assetlib::AssetState::Ready) {
                missing.insert(t.path);
                continue;
            }
            // Only ship what actually exists: a registry row can outlive its
            // cooked output (a cleared cache, a partial sync), and shipping a
            // reference to a file that is not there just moves the failure.
            std::error_code ec;
            if (!fs::exists(cacheRoot / rec->cookedPath, ec)) {
                missing.insert(t.path);
                continue;
            }
            t.cooked = rec->cookedPath;
            shipped.insert(rec->cookedPath);
        }

        // Written to the DIST, never back into the cache. Cooked outputs are
        // materialized from the DDC as READ-ONLY HARDLINKS — the file in
        // .cache is the same inode as the content-addressed blob, so rewriting
        // it in place would either fail (as it did) or, worse, corrupt a store
        // entry that other projects and other machines share.
        std::error_code mec;
        fs::create_directories(outDir, mec);
        if (!assetlib::saveMaterial(ma, outDir / file.filename()))
            missing.insert(file.filename().string() + " (could not write to the package)");
    }

    out.cookedRel.assign(shipped.begin(), shipped.end());
    out.unresolved.assign(missing.begin(), missing.end());
    return out;
}

} // namespace pkg
