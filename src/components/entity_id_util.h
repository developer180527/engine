#pragma once
#include <flecs.h>
#include <random>
#include <unordered_map>
#include <vector>
#include "components/entity_id.h"
#include "components/name.h"

// Random non-zero 64-bit id. Collision probability is negligible for scene
// sizes; avoids a per-project counter.
inline uint64_t generateEntityId() {
    static std::mt19937_64 rng{ std::random_device{}() };
    static std::uniform_int_distribution<uint64_t> dist;
    uint64_t v = 0;
    while (v == 0) v = dist(rng);
    return v;
}

// Return the entity's id, assigning one if absent. SAFE ONLY outside flecs
// query iteration (it may add a component). For iteration contexts use
// assignMissingIds() first, then read with try_get.
inline uint64_t ensureEntityId(flecs::entity e) {
    if (const EntityId* id = e.try_get<EntityId>())
        if (id->value != 0) return id->value;
    uint64_t v = generateEntityId();
    e.set<EntityId>({v});
    return v;
}

// Assign ids to every named entity that lacks one. Collects first, then sets
// outside iteration — safe to call before a serialization query.
inline void assignMissingIds(flecs::world& w) {
    std::vector<flecs::entity> need;
    w.query_builder<const Name>().build()
        .each([&](flecs::entity e, const Name&) {
            const EntityId* id = e.try_get<EntityId>();
            if (!id || id->value == 0) need.push_back(e);
        });
    for (auto e : need) e.set<EntityId>({ generateEntityId() });
}

// ── EntityIdIndex — the thing to use instead of findById in a LOOP ──────────
//
// findById below is O(n). Its comment used to say "used by load (one pass)", and
// that was the bug: scene load called it PER ENTITY as a collision check, so
// loading n entities cost O(n^2). Sampling a 50 000-object load put **97.6% of the
// whole 22 seconds** inside findById, reached from EntitySerde::createEntity.
//
// Build this once per load, then ask it. Seeded from the world so pre-existing
// entities still collide, and inserted into as entities are created so ids
// assigned within one load collide with each other too — both of which the
// per-entity findById gave for free and which a naive map would lose.
struct EntityIdIndex {
    std::unordered_map<uint64_t, flecs::entity> map;

    // One query over the world. Call before the create loop.
    void build(flecs::world& w) {
        map.clear();
        w.query_builder<const EntityId>().build()
            .each([&](flecs::entity e, const EntityId& eid) {
                if (eid.value) map.emplace(eid.value, e);
            });
    }
    bool taken(uint64_t id) const {
        if (id == 0) return true;                  // 0 is never a valid id
        auto it = map.find(id);
        return it != map.end() && it->second.is_alive();
    }
    void add(uint64_t id, flecs::entity e) { if (id) map[id] = e; }

    // The wanted id if it is free, otherwise a freshly generated free one.
    // Returns 0 never.
    uint64_t unique(uint64_t wanted) {
        uint64_t id = wanted;
        while (taken(id)) id = generateEntityId();
        return id;
    }
};

// Resolve an entity by stable id. O(n) — fine for a ONE-OFF lookup (undo, an
// editor click). NEVER call it in a loop over entities: use EntityIdIndex, or the
// loop is quadratic. See the note above.
inline flecs::entity findById(flecs::world& w, uint64_t id) {
    if (id == 0) return flecs::entity{};
    flecs::entity found{};
    w.query_builder<const EntityId>().build()
        .each([&](flecs::entity e, const EntityId& eid) {
            if (!found && eid.value == id) found = e;
        });
    return found;
}
