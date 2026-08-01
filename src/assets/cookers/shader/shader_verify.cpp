#include "assets/cookers/shader/shader_verify.h"

#include <set>

namespace shadercook {

std::string VerifyResult::joined(const char* sep) const {
    std::string s;
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) s += sep;
        s += errors[i];
    }
    return s;
}

namespace {
// Uniforms are matched across both stages: a parameter may live in either.
const ReflectedUniform* lookup(const Reflection& vs, const Reflection& fs,
                               const std::string& name) {
    if (const auto* u = fs.find(name)) return u;
    return vs.find(name);
}
} // namespace

VerifyResult verifyInterface(const std::vector<assetlib::ShaderParam>& params,
                             const std::vector<assetlib::ShaderSampler>& samplers,
                             const Reflection& vs, const Reflection& fs) {
    VerifyResult r;

    if (!vs.ok) r.errors.push_back("vertex reflection failed: " + vs.error);
    if (!fs.ok) r.errors.push_back("fragment reflection failed: " + fs.error);
    if (!r.errors.empty()) return r;

    std::set<std::string> declared;

    for (const auto& p : params) {
        declared.insert(p.uniform);
        const ReflectedUniform* u = lookup(vs, fs, p.uniform);
        if (!u) {
            r.errors.push_back("parameter \"" + p.name + "\" targets uniform \""
                             + p.uniform + "\", which the compiled shader does "
                               "not declare");
            continue;
        }
        if (u->kind == UniformKind::Sampler) {
            r.errors.push_back("parameter \"" + p.name + "\" targets \"" + p.uniform
                             + "\", which is a sampler, not a value uniform");
            continue;
        }
        // regCount is in vec4 registers; each holds 4 floats. The declared
        // param must fit entirely inside them, or the tail of the write lands
        // in whatever uniform the driver placed next.
        const uint32_t floatsAvailable = (uint32_t)u->regCount * 4;
        const uint32_t end = p.offset + assetlib::paramComponents(p.type);
        if (end > floatsAvailable) {
            r.errors.push_back("parameter \"" + p.name + "\" ("
                             + assetlib::paramTypeName(p.type) + " at offset "
                             + std::to_string(p.offset) + ") runs past \""
                             + p.uniform + "\", which holds "
                             + std::to_string(floatsAvailable) + " floats");
        }
    }

    for (const auto& s : samplers) {
        declared.insert(s.uniform);
        const ReflectedUniform* u = lookup(vs, fs, s.uniform);
        if (!u) {
            r.errors.push_back("sampler \"" + s.name + "\" targets uniform \""
                             + s.uniform + "\", which the compiled shader does "
                               "not declare");
            continue;
        }
        if (u->kind != UniformKind::Sampler) {
            r.errors.push_back("sampler \"" + s.name + "\" targets \"" + s.uniform
                             + "\", which is a " + uniformKindName(u->kind)
                             + ", not a sampler");
        }
    }

    // The reverse direction is a WARNING, not an error. A shader legitimately
    // has uniforms the engine drives itself — u_lights, u_shadowMtx, u_camPos —
    // and those are not material parameters. Reporting them as errors would
    // make the check unusable; reporting them not at all would hide the case
    // where an author added a uniform and forgot to expose it.
    for (const auto* refl : { &vs, &fs }) {
        for (const auto& u : refl->uniforms) {
            if (u.kind == UniformKind::End) continue;
            if (declared.count(u.name)) continue;
            r.warnings.push_back("shader declares \"" + u.name + "\" ("
                               + uniformKindName(u.kind)
                               + "), not exposed as a material parameter");
        }
    }

    r.ok = r.errors.empty();
    return r;
}

} // namespace shadercook
