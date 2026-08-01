#include "assets/cookers/shader/shader_manifest.h"

#include <nlohmann/json.hpp>

#include <set>

using nlohmann::json;

namespace shadercook {

std::vector<std::string> ShaderManifest::definesFor(uint32_t mask) const {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < (uint32_t)features.size(); ++i)
        if (mask & (1u << i)) out.push_back(features[i].define);
    return out;
}

namespace {

// Read `default` into 4 floats. Accepts a bare number (scalar params) or an
// array. A shorter array leaves the rest zero, which is what a vec3 default
// written as [1,1,1] should mean.
bool readDefaults(const json& j, uint32_t comps, float (&out)[4],
                  std::string& err) {
    if (j.is_number()) {
        // A scalar default for a multi-component param is almost always a
        // mistake, but splatting is the useful reading for e.g. a grey colour.
        const float v = j.get<float>();
        for (uint32_t i = 0; i < comps; ++i) out[i] = v;
        return true;
    }
    if (!j.is_array()) { err = "default must be a number or an array"; return false; }
    if (j.size() > comps) {
        err = "default has " + std::to_string(j.size()) + " values but the type holds "
            + std::to_string(comps);
        return false;
    }
    for (size_t i = 0; i < j.size(); ++i) {
        if (!j[i].is_number()) { err = "default values must be numbers"; return false; }
        out[i] = j[i].get<float>();
    }
    return true;
}

} // namespace

ManifestParseResult parseShaderManifest(const std::string& text,
                                        const std::filesystem::path& baseDir,
                                        ShaderManifest& out) {
    ManifestParseResult r;
    out = {};

    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        r.error = std::string("not valid JSON: ") + e.what();
        return r;
    }
    if (!j.is_object()) { r.error = "top level must be an object"; return r; }

    out.name = j.value("name", std::string{});
    if (out.name.empty()) { r.error = "missing \"name\""; return r; }

    const std::string vs = j.value("vertex",   std::string{});
    const std::string fs = j.value("fragment", std::string{});
    if (vs.empty()) { r.error = "missing \"vertex\""; return r; }
    if (fs.empty()) { r.error = "missing \"fragment\""; return r; }
    const std::string vd = j.value("varying", std::string{"varying.def.sc"});

    out.vertexPath   = baseDir / vs;
    out.fragmentPath = baseDir / fs;
    out.varyingPath  = baseDir / vd;

    // ── features ────────────────────────────────────────────────────────────
    if (j.contains("features")) {
        if (!j["features"].is_array()) { r.error = "\"features\" must be an array"; return r; }
        std::set<std::string> seen;
        for (const auto& fj : j["features"]) {
            ManifestFeature f;
            if (fj.is_string()) {
                f.name = fj.get<std::string>();
            } else if (fj.is_object()) {
                f.name   = fj.value("name",   std::string{});
                f.define = fj.value("define", std::string{});
            } else {
                r.error = "each feature must be a string or an object";
                return r;
            }
            if (f.name.empty())   { r.error = "a feature has an empty name"; return r; }
            if (f.define.empty()) f.define = f.name;
            // Duplicates would silently collapse two bit positions into one
            // define, so half the variant matrix would be identical bytecode
            // under different masks.
            if (!seen.insert(f.name).second) {
                r.error = "duplicate feature \"" + f.name + "\"";
                return r;
            }
            out.features.push_back(std::move(f));
        }
        if (out.features.size() > assetlib::kMaxShaderFeatures) {
            r.error = "too many features (" + std::to_string(out.features.size())
                    + " > " + std::to_string(assetlib::kMaxShaderFeatures)
                    + "); the variant matrix is 2^n per profile";
            return r;
        }
    }

    // ── parameters ──────────────────────────────────────────────────────────
    std::set<std::string> paramNames;
    if (j.contains("parameters")) {
        if (!j["parameters"].is_array()) { r.error = "\"parameters\" must be an array"; return r; }
        for (const auto& pj : j["parameters"]) {
            if (!pj.is_object()) { r.error = "each parameter must be an object"; return r; }
            assetlib::ShaderParam p;
            p.name = pj.value("name", std::string{});
            if (p.name.empty()) { r.error = "a parameter has no name"; return r; }
            if (!paramNames.insert(p.name).second) {
                r.error = "duplicate parameter \"" + p.name + "\"";
                return r;
            }
            const std::string type = pj.value("type", std::string{});
            if (!assetlib::paramTypeFromName(type, p.type)) {
                r.error = "parameter \"" + p.name + "\" has unknown type \"" + type + "\"";
                return r;
            }
            p.uniform = pj.value("uniform", std::string{});
            if (p.uniform.empty()) {
                r.error = "parameter \"" + p.name + "\" has no \"uniform\"";
                return r;
            }
            p.offset = pj.value("offset", 0u);
            // A param must fit inside the vec4 register it starts in. Straddling
            // two registers is not a packing subtlety — the second half lands in
            // an unrelated uniform slot and corrupts whatever lives there.
            const uint32_t comps = assetlib::paramComponents(p.type);
            if ((p.offset % 4) + comps > 4) {
                r.error = "parameter \"" + p.name + "\" (" + type + ", offset "
                        + std::to_string(p.offset) + ") straddles a vec4 boundary";
                return r;
            }
            if (pj.contains("default")) {
                std::string err;
                if (!readDefaults(pj["default"], comps, p.defaults, err)) {
                    r.error = "parameter \"" + p.name + "\": " + err;
                    return r;
                }
            }
            out.params.push_back(std::move(p));
        }
    }

    // Two parameters writing the same float is always a bug — one silently
    // overwrites the other depending on iteration order.
    for (size_t a = 0; a < out.params.size(); ++a) {
        const auto& pa = out.params[a];
        const uint32_t aEnd = pa.offset + assetlib::paramComponents(pa.type);
        for (size_t b = a + 1; b < out.params.size(); ++b) {
            const auto& pb = out.params[b];
            if (pa.uniform != pb.uniform) continue;
            const uint32_t bEnd = pb.offset + assetlib::paramComponents(pb.type);
            if (pa.offset < bEnd && pb.offset < aEnd) {
                r.error = "parameters \"" + pa.name + "\" and \"" + pb.name
                        + "\" overlap in " + pa.uniform;
                return r;
            }
        }
    }

    // ── samplers ────────────────────────────────────────────────────────────
    std::set<std::string> samplerNames;
    std::set<uint32_t>    stages;
    if (j.contains("samplers")) {
        if (!j["samplers"].is_array()) { r.error = "\"samplers\" must be an array"; return r; }
        for (const auto& sj : j["samplers"]) {
            if (!sj.is_object()) { r.error = "each sampler must be an object"; return r; }
            assetlib::ShaderSampler s;
            s.name = sj.value("name", std::string{});
            if (s.name.empty()) { r.error = "a sampler has no name"; return r; }
            if (!samplerNames.insert(s.name).second) {
                r.error = "duplicate sampler \"" + s.name + "\"";
                return r;
            }
            s.uniform = sj.value("uniform", std::string{});
            if (s.uniform.empty()) {
                r.error = "sampler \"" + s.name + "\" has no \"uniform\"";
                return r;
            }
            s.stage    = sj.value("stage", 0u);
            s.fallback = sj.value("fallback", std::string{});
            // Two samplers on one stage means the second bind wins and the
            // first texture is simply never sampled.
            if (!stages.insert(s.stage).second) {
                r.error = "sampler \"" + s.name + "\" reuses stage "
                        + std::to_string(s.stage);
                return r;
            }
            out.samplers.push_back(std::move(s));
        }
    }

    r.ok = true;
    return r;
}

} // namespace shadercook
