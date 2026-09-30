#pragma once
// ── UnresolvedMesh — an authored mesh reference the loader could not resolve ──
//
// LOADING NEVER DELETES AUTHORED DATA (WO-029, scene-service contract).
//
// A scene entity's "meshRenderer" object is a REFERENCE: an asset uuid and path,
// maybe a cooked path. The loader turns it into a MeshRenderer handle. When it
// cannot — the file is missing, the cooked load failed, the glTF import failed,
// the build has no importer for it, or an async import has not finished — the
// entity used to get no MeshRenderer at all. The save path only writes what a
// MeshRenderer holds, so the NEXT SAVE wrote the entity without its mesh, and
// the reference was gone for good. That is how fps_shooter lost `house`,
// `covered_car_2k` and `pistol_without_mag (2)`: they saved as a name and a
// transform, still selectable, rendering nothing, and no log said why.
//
// So the loader keeps the authored JSON here, verbatim, from the moment it
// starts resolving until a MeshRenderer is actually set. Save writes it back
// unchanged. The same idea as reflected::ReflectedPending, which keeps a kit
// component's blob until its type registers.
//
// ── RULES ───────────────────────────────────────────────────────────────────
//  * A MeshRenderer on the entity WINS: save ignores this component when one is
//    present. The loader removes this component whenever it sets a MeshRenderer.
//  * Nothing else in the engine assigns a mesh to an EXISTING entity or removes
//    a MeshRenderer today (every editor path creates a new entity). If one ever
//    does, it must also remove UnresolvedMesh — otherwise assign-then-remove
//    would resurrect the old reference on the next save.
//  * Memory snapshots (undo, Snapshot Play) carry it too, under "unresolved";
//    otherwise stopping Play would erase the reference just as surely.
//  * Not simulation state (SimExempt, runtime/sim_classification.cpp) and never
//    reflected: it has no meta, so the generic serde cannot see it.
#include <string>
#include <nlohmann/json.hpp>

struct UnresolvedMesh {
    std::string json;      // the entity's "meshRenderer" object, exactly as authored
    std::string reason;    // why it is not a mesh yet — shown in the Inspector
    bool        pending = false;   // an async import is still in flight
};

namespace unresolved_mesh {
// The authored path, for display: "path" if the reference has one, else the
// cooked path, else the asset uuid, else "(no path)". Best effort — the JSON is the
// source of truth, and a malformed one still round-trips untouched.
inline std::string describe(const UnresolvedMesh& u) {
    const auto j = nlohmann::json::parse(u.json, nullptr, /*allow_exceptions*/ false);
    if (j.is_object())
        for (const char* k : {"path", "cookedPath", "asset"})
            if (auto it = j.find(k); it != j.end() && it->is_string()
                                     && !it->get<std::string>().empty())
                return it->get<std::string>();
    return "(no path)";
}
}  // namespace unresolved_mesh
