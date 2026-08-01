#include "assets/cookers/material/material_manifest.h"

#include <nlohmann/json.hpp>

#include <set>

using nlohmann::json;

namespace matcook {

ManifestParseResult parseMaterialManifest(const std::string& text,
                                          MaterialManifest& out) {
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

    out.shaderRef = j.value("shader", std::string{});
    if (out.shaderRef.empty()) {
        // Defaulting to the standard shader was tempting, but a material whose
        // shader reference was lost in a bad edit would then silently render as
        // something else rather than failing.
        r.error = "missing \"shader\" — a material must name the shader it instances";
        return r;
    }
    out.name        = j.value("name", std::string{});
    out.doubleSided = j.value("doubleSided", false);

    if (j.contains("parameters")) {
        if (!j["parameters"].is_object()) {
            r.error = "\"parameters\" must be an object keyed by parameter name";
            return r;
        }
        for (const auto& [key, val] : j["parameters"].items()) {
            AuthoredValue v;
            v.name = key;
            if (val.is_number()) {
                v.v[0] = val.get<float>();
                v.count = 1;
            } else if (val.is_array()) {
                if (val.empty() || val.size() > 4) {
                    r.error = "parameter \"" + key + "\" must have 1 to 4 values";
                    return r;
                }
                for (size_t i = 0; i < val.size(); ++i) {
                    if (!val[i].is_number()) {
                        r.error = "parameter \"" + key + "\" has a non-numeric value";
                        return r;
                    }
                    v.v[i] = val[i].get<float>();
                }
                v.count = (uint32_t)val.size();
            } else {
                r.error = "parameter \"" + key + "\" must be a number or an array";
                return r;
            }
            out.input.values.push_back(std::move(v));
        }
    }

    if (j.contains("textures")) {
        if (!j["textures"].is_object()) {
            r.error = "\"textures\" must be an object keyed by sampler name";
            return r;
        }
        for (const auto& [key, val] : j["textures"].items()) {
            if (!val.is_string()) {
                r.error = "texture \"" + key + "\" must be a path string";
                return r;
            }
            out.input.textures.push_back({ key, val.get<std::string>() });
        }
    }

    if (j.contains("features")) {
        if (!j["features"].is_array()) {
            r.error = "\"features\" must be an array of feature names";
            return r;
        }
        std::set<std::string> seen;
        for (const auto& f : j["features"]) {
            if (!f.is_string()) {
                r.error = "each feature must be a string";
                return r;
            }
            const auto s = f.get<std::string>();
            // Harmless to OR a bit twice, but a duplicate always means the
            // author thinks it does something it doesn't.
            if (!seen.insert(s).second) {
                r.error = "duplicate feature \"" + s + "\"";
                return r;
            }
            out.input.features.push_back(s);
        }
    }

    r.ok = true;
    return r;
}

} // namespace matcook
