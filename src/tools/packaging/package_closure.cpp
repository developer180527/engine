#include "tools/packaging/package_closure.h"

#include <assetlib/mesh_asset.h>
#include <assetlib/scene_asset.h>
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

} // namespace pkg
