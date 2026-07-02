#pragma once
// ── Reflected component (de)serialization ────────────────────────────────────
// The GENERIC half of scene serde, driven by flecs meta (the reflection
// backbone — see components/meta_registry.h). Any component registered with
// `.member<>()` (i.e. it has EcsStruct) that is NOT in the hand-written
// EntitySerde table is saved/loaded automatically under the entity's
// "reflected" sub-object, keyed by the component's full path:
//
//   "reflected": { "combat::Health": { "current": 50, "max": 100 } }
//
// The load-order problem — kit components register AFTER scenes load (kits
// load at Play) — is solved by DEFERRED APPLICATION: a blob whose type isn't
// registered yet is stashed on the entity in ReflectedPending, and
// applyPending() applies it once the type appears. Pending blobs re-emit on
// save, so a component whose kit is disabled round-trips losslessly instead
// of being silently dropped.
//
// One registration site (kit or engine) now drives scene serde, the generic
// Inspector section, the + Add Component menu (via EditorAddable), and the
// Lua FFI schemas — add a component once, every consumer picks it up.
#include <flecs.h>
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>
#include "core/logger.h"

namespace reflected {

// Blobs waiting for their component type to be registered (kit not loaded yet).
// Never itself reflected (no meta); survives snapshots via re-emission.
struct ReflectedPending {
    std::map<std::string, std::string> blobs;   // component path -> JSON object
};

// Hand-written EntitySerde table + engine internals + member-only math types —
// these are serialized (or deliberately not) by the explicit table, never here.
inline bool isHandwritten(const std::string& path) {
    static const char* k[] = {
        "Transform", "Name", "MeshRenderer", "Camera", "Spinner", "RigidBody",
        "CollisionEvents", "ScriptComponent", "EntityId", "CharacterController",
        "Light", "SkinnedMesh", "Animator",
        "PhysicsServiceRef", "AudioServiceRef",
        "bx::Vec3", "bx::Quaternion",
    };
    for (const char* s : k)
        if (path == s) return true;
    return path.rfind("reflected::", 0) == 0     // our own bookkeeping
        || path.rfind("flecs", 0) == 0;          // flecs builtins
}

// Component path without the root prefix: "combat::Health". Symmetric with
// lookupPath below — this string is the on-disk identity of the component.
inline std::string componentPath(flecs::entity comp) {
    return comp.path("::", "").c_str();
}
inline flecs::entity lookupPath(flecs::world& w, const std::string& path) {
    return flecs::entity(w,
        ecs_lookup_path_w_sep(w, 0, path.c_str(), "::", "", true));
}

// get-or-add a component by id (this flecs's ecs_ensure_id wants the size,
// which lives on the component entity's EcsComponent).
inline void* ensurePtr(flecs::world& w, flecs::entity e, flecs::entity comp) {
    const EcsComponent* c = static_cast<const EcsComponent*>(
        ecs_get_id(w, comp, ecs_id(EcsComponent)));
    if (!c || c->size <= 0) return nullptr;
    return ecs_ensure_id(w, e, comp, (size_t)c->size);
}

// Is this id a reflectable data component on an entity?
inline bool isReflectable(flecs::id id) {
    if (id.is_pair()) return false;
    flecs::entity comp = id.entity();
    return comp.is_valid() && comp.has<flecs::Struct>()
        && !isHandwritten(componentPath(comp));
}

// ── Save: live reflected components + still-pending blobs ───────────────────
inline void save(flecs::entity e, nlohmann::json& out) {
    flecs::world w = e.world();
    e.each([&](flecs::id id) {
        if (!isReflectable(id)) return;
        flecs::entity comp = id.entity();
        const void* ptr = ecs_get_id(w, e, comp);
        if (!ptr) return;
        char* json = ecs_ptr_to_json(w, comp, ptr);
        if (!json) return;
        try { out[componentPath(comp)] = nlohmann::json::parse(json); }
        catch (...) {}
        ecs_os_free(json);
    });
    // Types whose kit isn't loaded: keep their data alive across save/load.
    if (const ReflectedPending* p = e.try_get<ReflectedPending>())
        for (const auto& [path, blob] : p->blobs)
            try { out[path] = nlohmann::json::parse(blob); } catch (...) {}
}

// ── Load: apply what resolves now, stash the rest ────────────────────────────
inline bool applyBlob(flecs::world& w, flecs::entity e, flecs::entity comp,
                      const std::string& blob) {
    void* ptr = ensurePtr(w, e, comp);
    if (!ptr) return false;
    if (!ecs_ptr_from_json(w, comp, ptr, blob.c_str(), nullptr)) {
        LOG_WARN("Reflected", "bad blob for %s — keeping defaults",
                 componentPath(comp).c_str());
    }
    ecs_modified_id(w, e, comp);
    return true;
}

inline void load(flecs::entity e, const nlohmann::json& in) {
    flecs::world w = e.world();
    for (auto it = in.begin(); it != in.end(); ++it) {
        const std::string& path = it.key();
        const std::string  blob = it.value().dump();
        flecs::entity comp = lookupPath(w, path);
        if (comp.is_valid() && comp.has<flecs::Struct>()) {
            applyBlob(w, e, comp, blob);
        } else {
            if (!e.has<ReflectedPending>()) e.set<ReflectedPending>({});
            e.get_mut<ReflectedPending>().blobs[path] = blob;
        }
    }
}

// ── Deferred application ─────────────────────────────────────────────────────
// Call whenever new component types may have appeared (after kits attach /
// sim-start / a mid-play kit load). Applies every blob that now resolves.
inline void applyPending(flecs::world& w) {
    std::vector<flecs::entity> pending;   // structural changes outside the query
    w.query_builder<ReflectedPending>().build()
        .each([&](flecs::entity e, ReflectedPending&) { pending.push_back(e); });

    for (flecs::entity e : pending) {
        if (!e.is_alive() || !e.has<ReflectedPending>()) continue;
        auto blobs = e.get<ReflectedPending>().blobs;   // copy — we mutate below
        for (auto it = blobs.begin(); it != blobs.end(); ) {
            flecs::entity comp = lookupPath(w, it->first);
            if (comp.is_valid() && comp.has<flecs::Struct>()
                && applyBlob(w, e, comp, it->second)) {
                LOG_INFO("Reflected", "applied %s to entity %llu",
                         it->first.c_str(), (unsigned long long)e.id());
                it = blobs.erase(it);
            } else ++it;
        }
        if (blobs.empty()) e.remove<ReflectedPending>();
        else               e.get_mut<ReflectedPending>().blobs = std::move(blobs);
    }
}

} // namespace reflected
