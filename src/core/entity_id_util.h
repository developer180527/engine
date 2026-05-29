#pragma once
#include <flecs.h>
#include <random>
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

// Resolve an entity by stable id. O(n) — used by load (one pass) and undo
// (infrequent), not per-frame.
inline flecs::entity findById(flecs::world& w, uint64_t id) {
    if (id == 0) return flecs::entity{};
    flecs::entity found{};
    w.query_builder<const EntityId>().build()
        .each([&](flecs::entity e, const EntityId& eid) {
            if (!found && eid.value == id) found = e;
        });
    return found;
}
