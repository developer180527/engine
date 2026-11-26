#pragma once
// ── Extensions a project declares (WO-071, DR-0035) ─────────────────────────
//
// An extension is a namespace of components that something other than the engine
// core provides: a provider's (`apollo.Thruster`), or a kit's (`combat::Health`).
// A scene using one is only fully readable where it is present. Without it the
// components are kept and saved back unchanged (DR-0034), but a game whose
// behaviour depends on them must not run as if they were not there.
//
// So the project says which it needs, as glTF does with extensionsRequired and
// extensionsUsed:
//
//   "extensions": { "required": ["combat"], "used": ["apollo"] }
//
//   required  missing at simulation start: refused, by name, with what this build
//             has. Play does not start; the player does not run.
//   used      missing: one warning each, and the scene runs without them.
//
// Checked when the simulation starts, after the project's kits attach: kits are
// loaded at Play, so that is the first point at which "missing" is known. The
// editor opens a project whatever it lacks, because authoring around a missing
// extension is how it gets fixed.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <flecs.h>

#include "scene/component_schema.h"

namespace extensions {

// "apollo.Thruster" -> "apollo"; "combat::Health" -> "combat"; "Transform" -> "".
inline std::string namespaceOf(const std::string& componentName) {
    const std::size_t colons = componentName.find("::");
    const std::size_t dot    = componentName.find('.');
    const std::size_t at     = colons < dot ? colons : dot;
    return at == std::string::npos ? std::string{} : componentName.substr(0, at);
}

// Namespaces the engine registers flecs components under for its own
// bookkeeping, not extensions anything provides: flecs' own, the math types
// (bx::Vec3), reflected serde's pending blobs, the sim hash's classification.
inline bool isEngineInternal(const std::string& ns) {
    return ns == "flecs" || ns == "bx" || ns == "reflected" || ns == "simhash";
}

// Every namespace a component is registered under: the schema registry's, and
// the world's own components (a kit's, registered through flecs meta).
inline std::set<std::string> available(flecs::world& w,
                                       const EntitySerde::SchemaRegistry& schemas) {
    std::set<std::string> out;
    for (const EntitySerde::ComponentSchema& s : schemas.all())
        if (std::string ns = namespaceOf(s.name); !ns.empty()) out.insert(ns);
    w.each([&](flecs::entity e, flecs::Component&) {
        const std::string path = e.path("::", "").c_str();
        std::string ns = namespaceOf(path);
        if (!ns.empty() && !isEngineInternal(ns)) out.insert(ns);
    });
    return out;
}

struct Missing {
    std::vector<std::string> required;   // refuse
    std::vector<std::string> used;       // warn
    std::string              have;       // what there is, for the message: "combat, core"
};

inline Missing check(flecs::world& w, const EntitySerde::SchemaRegistry& schemas,
                     const std::vector<std::string>& required,
                     const std::vector<std::string>& used) {
    const std::set<std::string> have = available(w, schemas);
    Missing m;
    for (const std::string& r : required) if (!have.count(r)) m.required.push_back(r);
    for (const std::string& u : used)
        if (!have.count(u) && std::find(required.begin(), required.end(), u) == required.end())
            m.used.push_back(u);
    for (const std::string& h : have) { if (!m.have.empty()) m.have += ", "; m.have += h; }
    return m;
}

}  // namespace extensions
