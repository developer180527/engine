#pragma once
// ── Resolve — authored values against a shader's declared interface ─────────
//
// ONE concern: given what a material SAYS and what a shader DECLARES, produce
// the finished uniform blocks and texture bindings — or explain why not.
//
// This is where "materials are data" actually happens, and it is deliberately a
// pure function so it can be tested without a cooker, a compiler, or a file.
//
// Three rules it enforces, each catching a mistake that is otherwise invisible
// until someone looks at the render and frowns:
//
//   • a parameter the shader never declared is an ERROR, not an ignored key.
//     Typing "roughtness" should fail the cook, not silently do nothing.
//   • a value of the wrong arity is an ERROR (a scalar into a vec4 is a real
//     authoring intent — splat — but 3 floats into a float is a mistake).
//   • unset parameters are FILLED WITH THE SHADER'S DEFAULTS, never left as
//     whatever the previous draw wrote into that register.
#include <assetlib/material_asset.h>
#include <assetlib/shader_asset.h>

#include <string>
#include <vector>

namespace matcook {

// One authored value, straight from the .material. Kept as up-to-4 floats plus
// a count so arity can be checked against the declared type.
struct AuthoredValue {
    std::string name;
    float       v[4] = { 0, 0, 0, 0 };
    uint32_t    count = 0;
};

struct AuthoredTexture {
    std::string name;    // material-facing sampler name, e.g. "baseColor"
    std::string path;    // project-relative source path
};

struct ResolveInput {
    std::vector<AuthoredValue>   values;
    std::vector<AuthoredTexture> textures;
    std::vector<std::string>     features;   // feature names to enable
};

struct ResolveResult {
    bool ok = false;
    std::vector<std::string> errors;
    std::string joined(const char* sep = "\n  ") const;
};

// `params` / `samplers` / `features` come from the .shader manifest. Fills
// out.uniforms, out.textures and out.featureMask.
ResolveResult resolveMaterial(const std::vector<assetlib::ShaderParam>& params,
                              const std::vector<assetlib::ShaderSampler>& samplers,
                              const std::vector<std::string>& features,
                              const ResolveInput& in,
                              assetlib::MaterialAsset& out);

} // namespace matcook
