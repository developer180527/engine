#pragma once
// ── .material manifest — the authored source format ─────────────────────────
//
// ONE concern: parse a `.material` file. No shader lookup, no resolution, no
// validation against an interface — that is material_resolve's job, and keeping
// them apart is what lets each be tested on its own.
//
//   {
//     "shader": "shaders/standard.shader",
//     "features": ["SKINNED"],
//     "parameters": { "roughness": 0.25, "baseColorFactor": [0.9, 0.2, 0.1, 1] },
//     "textures":   { "baseColor": "textures/rust_albedo.png" },
//     "doubleSided": false
//   }
//
// `parameters` and `textures` are OBJECTS keyed by the names the shader
// declared, so an author writes what they mean and the cooker checks it against
// the shader rather than against a hardcoded list in C++.
#include "assets/cookers/material/material_resolve.h"

#include <filesystem>
#include <string>

namespace matcook {

struct MaterialManifest {
    std::string  name;
    std::string  shaderRef;      // as authored, project-relative
    bool         doubleSided = false;
    ResolveInput input;          // values / textures / features
};

struct ManifestParseResult {
    bool        ok = false;
    std::string error;
};

ManifestParseResult parseMaterialManifest(const std::string& text,
                                          MaterialManifest& out);

} // namespace matcook
