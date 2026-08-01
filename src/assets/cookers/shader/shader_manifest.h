#pragma once
// ── .shader manifest — the authored source format ───────────────────────────
//
// ONE concern: parse and validate a `.shader` file into a structure the cooker
// can act on. No compiling, no file writing, no shaderc.
//
// A `.shader` is JSON next to the `.sc` sources it names:
//
//   {
//     "name":     "standard",
//     "vertex":   "vs_triangle.sc",
//     "fragment": "fs_triangle.sc",
//     "varying":  "varying.def.sc",
//     "features": [ { "name": "SKINNED", "define": "SKINNED" } ],
//     "parameters": [
//       { "name": "baseColorFactor", "type": "color", "uniform": "u_colorFactor",
//         "offset": 0, "default": [1,1,1,1] },
//       { "name": "roughness", "type": "float", "uniform": "u_params",
//         "offset": 1, "default": 0.7 }
//     ],
//     "samplers": [
//       { "name": "baseColor", "uniform": "s_baseColor", "stage": 0,
//         "fallback": "white" }
//     ]
//   }
//
// `features` is a CLOSED list of preprocessor defines. That is the whole
// anti-bloat policy: a material selects a combination, it cannot invent one.
// The full matrix is 2^features per profile, which stays cookable precisely
// because the list is short and the engine owns it.
//
// Parsing is separated from cooking because it is the part that reads
// untrusted input — a `.shader` can come from a project, not just the engine —
// and because it is the part worth testing without a compiler on the machine.
#include <assetlib/shader_asset.h>

#include <filesystem>
#include <string>
#include <vector>

namespace shadercook {

struct ManifestFeature {
    std::string name;      // the identity a material selects by
    std::string define;    // what shaderc gets (-D); defaults to `name`
};

struct ShaderManifest {
    std::string name;
    std::filesystem::path vertexPath;     // resolved absolute
    std::filesystem::path fragmentPath;
    std::filesystem::path varyingPath;
    std::vector<ManifestFeature>          features;
    std::vector<assetlib::ShaderParam>    params;
    std::vector<assetlib::ShaderSampler>  samplers;

    // 1 << features.size() — every feature combination.
    uint32_t variantCount() const { return 1u << features.size(); }
    // The defines active for one mask, in declaration order.
    std::vector<std::string> definesFor(uint32_t featureMask) const;
};

struct ManifestParseResult {
    bool        ok = false;
    std::string error;      // human-readable, names the offending field
};

// `text` is the raw file; `baseDir` resolves the relative .sc paths.
//
// Validation is strict and rejects rather than repairing. A `.shader` with a
// typo'd uniform name should fail the cook loudly — the alternative is a
// material parameter that silently writes nowhere, which is invisible in the
// editor and wrong on screen.
ManifestParseResult parseShaderManifest(const std::string& text,
                                        const std::filesystem::path& baseDir,
                                        ShaderManifest& out);

} // namespace shadercook
