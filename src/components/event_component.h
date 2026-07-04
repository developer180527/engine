#pragma once
// ── Event components — one-shot ECS messages with a guaranteed lifetime ──────
//
// The problem this fixes (the "write-after-consumer" hazard):
//   Kits cooperate by writing components (combat::dealDamage writes a
//   DamageInbox; CombatKit drains it). But whether the consumer sees a message
//   the SAME tick it was written depends on broadcast ORDER — if CombatKit's
//   onUpdate runs before the gun's, the freshly-written inbox is missed and
//   sits until next tick, or worse is overwritten/cleared before anyone reads
//   it. Load order is not something a kit author should have to reason about.
//
// The fix: mark a component TYPE as an EVENT. The EventSweeper (runtime/
// event_sweeper.h) then runs a mark/sweep at the START of every fixed tick so
// that an event written ANYWHERE in tick N is guaranteed to still be present
// through the whole of tick N+1 — every consumer gets at least one full tick
// to observe it, regardless of who ran first. Unconsumed events then expire on
// their own; a consumer that drains (removes) the component short-circuits the
// wait. See event_sweeper.h for the exact lifecycle.
//
// Events vs. transient STATE: an event is a MESSAGE (fire-once, auto-expiring —
// damage dealt, a hit landed). Persistent-within-session state that a system
// sets and later clears itself (combat::Died, interpolation caches) is NOT an
// event: it lives until its owner removes it. Both are SerdeTransient (never
// authored/saved); only events are swept. Declare an event with
// events::declare<T>(world) — that also tags it SerdeTransient for you.
#include <flecs.h>
#include <vector>

#include "components/serde_transient.h"

// Tag added to a component TYPE (its component entity) to mark it an event.
struct EventComponent {};

// Relation used as a PAIR (EventStale, T): "event T on this entity has already
// survived one tick boundary unconsumed — sweep it at the next boundary." Per
// (entity, event-type) so an entity can carry several events of different ages.
struct EventStale {};

// Per-world singleton: the set of component ids declared as events, so the
// sweeper knows what to walk. Ids are world-local, hence per-world storage.
struct EventRegistry { std::vector<flecs::entity_t> types; };

namespace events {

// Declare component T as an event in world w (idempotent per world). Tags the
// type EventComponent + SerdeTransient and enrolls it with the sweeper.
template <typename T>
inline void declare(flecs::world& w) {
    flecs::entity ct = w.component<T>();
    if (ct.template has<EventComponent>()) return;   // already declared here
    ct.template add<EventComponent>().template add<SerdeTransient>();
    w.ensure<EventRegistry>().types.push_back(ct.id());
}

// CONSUME (drain) an event: the correct way for a system to remove an event it
// has handled. It clears both the component AND its staleness marker in one
// shot. Do NOT drain with a bare entity.remove<T>(): if the sweeper had already
// marked the event stale, the leftover (EventStale, T) pair would orphan and
// prematurely expire the NEXT event of type T on this entity. (Destroying the
// carrier entity is also safe — that takes the pair with it.) Removing a
// pair/component that isn't present is a harmless no-op.
template <typename T>
inline void consume(flecs::entity e) {
    flecs::world w = e.world();
    e.remove(ecs_pair(w.component<EventStale>(), w.component<T>()));
    e.template remove<T>();
}

} // namespace events
