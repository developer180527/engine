#include "assets/cookers/material/material_resolve.h"

#include <algorithm>
#include <map>

namespace matcook {

std::string ResolveResult::joined(const char* sep) const {
    std::string s;
    for (size_t i = 0; i < errors.size(); ++i) { if (i) s += sep; s += errors[i]; }
    return s;
}

ResolveResult resolveMaterial(const std::vector<assetlib::ShaderParam>& params,
                              const std::vector<assetlib::ShaderSampler>& samplers,
                              const std::vector<std::string>& features,
                              const ResolveInput& in,
                              assetlib::MaterialAsset& out) {
    ResolveResult r;
    out.uniforms.clear();
    out.textures.clear();
    out.featureMask = 0;

    // ── 1. size every block the shader's parameters touch ───────────────────
    // Blocks are built from the SHADER's declaration, not from what the
    // material happened to set. That is what makes an unset parameter fall
    // back to its default instead of inheriting the previous draw's register.
    std::map<std::string, std::vector<float>> blocks;
    for (const auto& p : params) {
        const uint32_t end = p.offset + assetlib::paramComponents(p.type);
        // Round up to a whole vec4 — bgfx uploads in vec4 registers.
        const size_t need = ((end + 3) / 4) * 4;
        auto& b = blocks[p.uniform];
        if (b.size() < need) b.resize(need, 0.0f);
    }
    // Defaults first, overrides second.
    for (const auto& p : params) {
        auto& b = blocks[p.uniform];
        const uint32_t comps = assetlib::paramComponents(p.type);
        for (uint32_t i = 0; i < comps; ++i) b[p.offset + i] = p.defaults[i];
    }

    // ── 2. apply what the material authored ─────────────────────────────────
    for (const auto& v : in.values) {
        const assetlib::ShaderParam* p = nullptr;
        for (const auto& cand : params) if (cand.name == v.name) { p = &cand; break; }
        if (!p) {
            // The typo case. Ignoring an unknown key is how a material ends up
            // looking wrong with nothing anywhere reporting a problem.
            std::string known;
            for (const auto& cand : params) {
                if (!known.empty()) known += ", ";
                known += cand.name;
            }
            r.errors.push_back("parameter \"" + v.name + "\" is not declared by "
                               "the shader (declared: "
                             + (known.empty() ? "none" : known) + ")");
            continue;
        }
        const uint32_t comps = assetlib::paramComponents(p->type);
        // A single number splatting across a vector is deliberate authoring
        // (grey = 0.5); anything else that doesn't match the arity is a
        // mistake, and silently truncating it would be the worst outcome.
        if (v.count != comps && v.count != 1) {
            r.errors.push_back("parameter \"" + v.name + "\" is "
                             + assetlib::paramTypeName(p->type) + " ("
                             + std::to_string(comps) + " values) but "
                             + std::to_string(v.count) + " were given");
            continue;
        }
        auto& b = blocks[p->uniform];
        for (uint32_t i = 0; i < comps; ++i)
            b[p->offset + i] = (v.count == 1) ? v.v[0] : v.v[i];
    }

    for (auto& [name, values] : blocks)
        out.uniforms.push_back({ name, std::move(values) });

    // ── 3. textures ─────────────────────────────────────────────────────────
    // Every DECLARED sampler produces a binding, whether or not the material
    // set it: an unset sampler still needs its fallback bound, or the stage
    // holds whatever texture the previous draw left there.
    for (const auto& s : samplers) {
        assetlib::MaterialTexture t;
        t.uniform  = s.uniform;
        t.stage    = s.stage;
        t.fallback = s.fallback;
        for (const auto& a : in.textures)
            if (a.name == s.name) { t.path = a.path; break; }
        out.textures.push_back(std::move(t));
    }
    for (const auto& a : in.textures) {
        bool declared = false;
        for (const auto& s : samplers) if (s.name == a.name) { declared = true; break; }
        if (!declared) {
            std::string known;
            for (const auto& s : samplers) {
                if (!known.empty()) known += ", ";
                known += s.name;
            }
            r.errors.push_back("texture \"" + a.name + "\" is not a sampler "
                               "declared by the shader (declared: "
                             + (known.empty() ? "none" : known) + ")");
        }
    }

    // ── 4. features ─────────────────────────────────────────────────────────
    for (const auto& want : in.features) {
        const auto it = std::find(features.begin(), features.end(), want);
        if (it == features.end()) {
            r.errors.push_back("feature \"" + want + "\" is not declared by the "
                               "shader — no such variant was cooked");
            continue;
        }
        out.featureMask |= 1u << (uint32_t)(it - features.begin());
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace matcook
