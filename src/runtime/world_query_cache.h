#pragma once
#include <flecs.h>

// ── WorldQueryCache ─────────────────────────────────────────────────────────
// Caches a flecs query per world. Query construction does real work
// (allocation + archetype matching) — building one per frame or per call is
// the kind of overhead that hides in profiles. This caches by world pointer
// and rebuilds only when the world changes.
//
// LIFETIME RULE: call reset() when a world you've queried gets destroyed
// (e.g. the play-mode snapshot world on simulation stop). A new world can be
// allocated at the SAME address, which would otherwise false-match against a
// dangling query. The runtime resets its caches in stopSimulation; anything
// holding its own cache against the sim world must do the same.
template <typename... Comps>
class WorldQueryCache {
public:
    flecs::query<Comps...>& get(flecs::world& w) {
        ecs_world_t* ptr = w.c_ptr();
        if (ptr != m_world) {
            m_query = w.template query_builder<Comps...>().build();
            m_world = ptr;
        }
        return m_query;
    }

    void reset() {
        m_world = nullptr;
        m_query = flecs::query<Comps...>{};
    }

private:
    ecs_world_t*           m_world = nullptr;
    flecs::query<Comps...> m_query{};
};
