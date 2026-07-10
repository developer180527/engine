#pragma once
// ── EventSweeper — the mark/sweep that gives events their one-tick lifetime ──
//
// Runs ONCE at the start of every fixed simulation tick, before any plugin
// broadcast. For each component type declared via events::declare<T>() it walks
// every entity carrying that event and applies a two-state age machine using a
// per-(entity, type) pair (EventStale, T):
//
//   fresh  (no (EventStale,T))  ─mark→  add (EventStale,T)      // survived to
//                                                               // its 1st boundary
//   stale  (has (EventStale,T)) ─sweep→ remove T + the pair     // expired
//
// Walk the timeline of an event written mid-tick N (at ANY point in the
// broadcast order):
//   • tick N     — present for every consumer whose broadcast ran AFTER the
//                  writer. Any of them may DRAIN it (remove T) → short-circuit.
//   • start N+1  — sweeper sees it fresh → marks it stale. T stays put.
//   • tick N+1   — present for the WHOLE tick, so every consumer (including
//                  ones that ran before the writer in N) gets a guaranteed
//                  look. The canonical consumer drains it here.
//   • start N+2  — if STILL unconsumed, sweeper sees it stale → removes it.
//
// So: every consumer is guaranteed at least one full tick with the event, no
// matter the load order, and nothing an event carries leaks past two ticks.
// Draining (removing the component) at consume time is the fast path — a
// consumed event never reaches the stale state and is gone immediately.
//
// Standalone + testable: sweep() needs nothing but a world holding an
// EventRegistry (see src/tools/event_test.cpp). Queries are cached against the
// live world and MUST be released via reset() before that world is destroyed
// (a query outliving its world = ecs_query_fini on freed memory).
#include <flecs.h>
#include <vector>

#include "components/event_component.h"

class EventSweeper {
public:
    // Advance every declared event one tick. Call at tick start, pre-broadcast.
    void sweep(flecs::world& w) {
        const EventRegistry* reg = w.try_get<EventRegistry>();
        if (!reg || reg->types.empty()) return;

        // (Re)build the per-type queries when the world or the event set
        // changed. Building queries every tick is measurable waste, so they're
        // cached. Compared by CONTENT, not count (audit M.3): unregister one
        // type + register another and the size stays equal while every cached
        // query silently points at the wrong component.
        if (w.c_ptr() != m_world || reg->types != m_cachedTypes)
            rebuild(w, *reg);

        const flecs::entity_t staleRel = w.component<EventStale>();

        // Structural churn (add/remove) is deferred so it's safe to issue while
        // iterating the very tables we mutate; flecs applies it at defer_end.
        const bool nested = w.is_deferred();
        if (!nested) w.defer_begin();
        for (size_t i = 0; i < m_queries.size(); ++i) {
            const flecs::id_t stalePair = ecs_pair(staleRel, reg->types[i]);
            m_queries[i].each([&](flecs::entity e) {
                if (e.has(stalePair)) {                 // stale → expire
                    e.remove(reg->types[i]);
                    e.remove(stalePair);
                } else {                                // fresh → mark
                    e.add(stalePair);
                }
            });
        }
        if (!nested) w.defer_end();
    }

    // Drop cached queries. Call before the world they were built against dies.
    void reset() { m_queries.clear(); m_cachedTypes.clear(); m_world = nullptr; }

private:
    void rebuild(flecs::world& w, const EventRegistry& reg) {
        m_queries.clear();
        m_queries.reserve(reg.types.size());
        for (flecs::entity_t tid : reg.types)
            m_queries.push_back(w.query_builder().with(tid).build());
        m_cachedTypes = reg.types;   // identity of what the queries match
        m_world = w.c_ptr();
    }

    std::vector<flecs::query<>>  m_queries;
    std::vector<flecs::entity_t> m_cachedTypes; // rebuild trigger (M.3)
    const flecs::world_t*        m_world = nullptr;
};
