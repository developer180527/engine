#include "assets/cookers/material/material_cooker.h"
#include "assets/cookers/material/material_manifest.h"
#include "assets/cookers/material/material_resolve.h"
#include "assets/cookers/shader/shader_manifest.h"
#include "core/logger.h"

#include <assetlib/ddc.h>
#include <assetlib/material_asset.h>

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace matcook;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Walk up from the material looking for project.json. The cooker contract hands
// over a source path and an output path but no project root, and a material's
// shader reference is written project-relative because that is what an author
// can actually type. Bounded so a material outside any project fails instead of
// walking to `/`.
fs::path findProjectRoot(const fs::path& from) {
    std::error_code ec;
    fs::path dir = from.parent_path();
    for (int depth = 0; depth < 16 && !dir.empty(); ++depth) {
        if (fs::exists(dir / "project.json", ec)) return dir;
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return {};
}

// Candidate locations for a shader reference, most specific first. Returned
// rather than resolved so the error message can list what was tried — "shader
// not found" without the search path is a miserable thing to debug.
std::vector<fs::path> shaderCandidates(const std::string& ref,
                                       const fs::path& materialPath) {
    std::vector<fs::path> out;
    out.push_back(materialPath.parent_path() / ref);
    const fs::path root = findProjectRoot(materialPath);
    if (!root.empty()) {
        out.push_back(root / ref);
        out.push_back(root / "assets" / ref);
    }
    return out;
}

fs::path resolveShader(const std::string& ref, const fs::path& materialPath) {
    std::error_code ec;
    for (const auto& c : shaderCandidates(ref, materialPath))
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    return {};
}

} // namespace

std::string MaterialCooker::settingsFingerprint(const assetlib::CookContext& ctx) const {
    MaterialManifest man;
    if (!parseMaterialManifest(slurp(ctx.sourcePath), man).ok) return "unparsed";

    const fs::path shader = resolveShader(man.shaderRef, ctx.sourcePath);
    if (shader.empty()) return "shader-missing:" + man.shaderRef;

    // Only the .shader manifest is hashed, not the .sc stage sources. This
    // material's output depends on the DECLARED INTERFACE (names, types,
    // offsets, defaults) — which lives entirely in the manifest. Shading code
    // changes don't alter a single byte here, and hashing them would re-cook
    // every material in the project on every shader edit.
    const std::string h = assetlib::blake3File(shader);
    return "shader=" + man.shaderRef + "@" + (h.empty() ? "unreadable" : h.substr(0, 16));
}

assetlib::CookResult MaterialCooker::cook(const assetlib::CookContext& ctx) {
    assetlib::CookResult res;

    const std::string text = slurp(ctx.sourcePath);
    if (text.empty()) {
        res.error = "cannot read " + ctx.sourcePath.string();
        return res;
    }

    MaterialManifest man;
    const auto parsed = parseMaterialManifest(text, man);
    if (!parsed.ok) {
        res.error = "invalid .material: " + parsed.error;
        return res;
    }

    const fs::path shaderPath = resolveShader(man.shaderRef, ctx.sourcePath);
    if (shaderPath.empty()) {
        std::string tried;
        for (const auto& c : shaderCandidates(man.shaderRef, ctx.sourcePath)) {
            if (!tried.empty()) tried += ", ";
            tried += c.string();
        }
        res.error = "shader \"" + man.shaderRef + "\" not found (tried: " + tried + ")";
        return res;
    }

    shadercook::ShaderManifest shader;
    const auto shaderParsed = shadercook::parseShaderManifest(
        slurp(shaderPath), shaderPath.parent_path(), shader);
    if (!shaderParsed.ok) {
        res.error = "referenced shader " + man.shaderRef + " is invalid: "
                  + shaderParsed.error;
        return res;
    }

    std::vector<std::string> featureNames;
    for (const auto& f : shader.features) featureNames.push_back(f.name);

    assetlib::MaterialAsset out;
    out.name = man.name.empty() ? ctx.sourcePath.stem().string() : man.name;
    out.shaderName  = shader.name;      // what the runtime resolves by
    out.shaderPath  = man.shaderRef;    // what a human edits
    out.doubleSided = man.doubleSided;

    const auto resolved = resolveMaterial(shader.params, shader.samplers,
                                          featureNames, man.input, out);
    if (!resolved.ok) {
        res.error = "material does not match shader \"" + shader.name + "\":\n  "
                  + resolved.joined();
        return res;
    }

    if (!assetlib::saveMaterial(out, ctx.outputPath)) {
        res.error = "failed to write " + ctx.outputPath.string();
        return res;
    }

    size_t floats = 0;
    for (const auto& u : out.uniforms) floats += u.values.size();
    LOG_INFO("MaterialCooker", "%s -> %s: %zu uniform block(s) / %zu float(s), "
             "%zu texture(s), features 0x%x",
             out.name.c_str(), shader.name.c_str(), out.uniforms.size(), floats,
             out.textures.size(), out.featureMask);

    res.success = true;
    return res;
}
