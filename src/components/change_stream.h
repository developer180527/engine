#pragma once
// ── change_stream — what changed in a component since you last looked ────────
// WO-072; docs/plans/engine-api.md §5 (marked writes, merged changes) and the
// draft's engSubscribeChanges / engReadChanges (engine-api-draft/eng/eng_world.h).
//
// Every provider that keeps its own copy of the world (the renderer's retained
// scene, physics bodies, audio voices, replication) needs to know what changed
// since it last looked. This is that, for any registered component, in the
// world itself: a provider subscribes to a component, reads ADDED / CHANGED /
// REMOVED per entity, and unsubscribes. The C world API (WO-073) is a thin
// layer over these calls.
//
// ── FREE WITH NO SUBSCRIBER (DR-0011) ───────────────────────────────────────
// Nothing exists until something subscribes: no observer, no allocation, and
// a writer's check is one load of the world's context pointer, which is null.
// After the last unsubscribe the observer is deleted again and a writer's
// check is a scan of the few ids ever watched. empty_game_budget_test holds
// the first half: an empty game never creates any of this.
//
// ── HOW A CHANGE IS SEEN ────────────────────────────────────────────────────
//   * Structural: adding and removing the component (and deleting the entity)
//     are seen by a flecs observer created at the first subscribe. So is
//     `set<T>()` and `modified<T>()` (OnSet). Nothing to do for a writer that
//     uses them.
//   * A write IN PLACE (a query's mutable column, get_mut, try_get_mut) is
//     invisible to flecs, so it is DECLARED: the writer marks what it wrote.
//     `Marks<T>` is the writer's handle, made once per pass outside its loop:
//       column(it)              every row of the batch changed  (ENG_ACCESS_WRITE)
//       rows(it, first, count)  only these rows changed         (ENG_ACCESS_WRITE_MARKED
//                                                                 + engMarkChanged)
//       entity(e)               one entity changed
//     Over-reporting is allowed (a provider may see a row that did not really
//     change); missing a write is the bug, and the renderer's rebuild-and-diff
//     check (ENGINE_RENDER_TABLE_DIFF=1) is the oracle that catches one.
//     Marks are made on the thread that owns the world: a parallel writer
//     collects what it wrote and marks after the join (the animator does).
//
// ── MERGED PER READ ─────────────────────────────────────────────────────────
// Between two reads an entity appears at most once per stream:
//     changed many times      -> CHANGED      added then changed   -> ADDED
//     changed then removed    -> REMOVED      added then removed   -> nothing
//     removed then added      -> CHANGED (it existed at the last read, and does now)
//
// ── IDS ARE NOT REUSED UNDER A READER ───────────────────────────────────────
// A stream reports the full 64-bit entity id, generation included. flecs
// recycles a deleted entity's index with the generation bumped, so the next
// entity in that slot has a DIFFERENT id: a REMOVED for the old one and an
// ADDED for the new one are two entries, never merged. The limit, stated: the
// generation is 16 bits, so a slot deleted and recreated 65 536 times between
// two reads would wrap.
//
// ── A NEW SUBSCRIBER SEES WHAT IS ALREADY THERE ─────────────────────────────
// subscribe() queues every entity that has the component as ADDED, so a
// provider can build its whole copy from the stream alone, and a provider
// that subscribes late is not a special case.
//
// The world's context pointer (ecs_set_ctx) holds this; nothing else in the
// engine uses it. It is freed after the world's entities are, so observers
// that fire while a world shuts down still find it, and ignore what they see.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <flecs.h>

namespace changes {

// The draft's EngChangeKind values, so the C API can pass these through.
enum class Kind : uint32_t { Added = 0, Changed = 1, Removed = 2 };

struct Change {
    flecs::entity_t entity = 0;
    Kind            kind   = Kind::Changed;
};

using StreamId = uint32_t;   // 0: no stream

class Streams {
public:
    // This world's streams, or null when nothing has ever subscribed in it.
    static Streams* of(const flecs::world_t* w) {
        return w ? static_cast<Streams*>(ecs_get_ctx(w)) : nullptr;
    }
    static Streams& ensure(flecs::world_t* w) {
        if (Streams* s = of(w)) return *s;
        auto* s = new Streams(w);
        ecs_set_ctx(w, s, [](void* p) { delete static_cast<Streams*>(p); });
        return *s;
    }

    // Unique per instance, for a holder that must notice its world was
    // replaced by another at the same address.
    uint64_t epoch() const { return m_epoch; }

    // A stream for `id` (a component, a tag, or a pair such as
    // (ChildOf, *)), with every entity that has it queued as ADDED. Not
    // inside a deferred block or a running query: it creates an observer.
    StreamId subscribe(flecs::id_t id) {
        Watch& wt = watchFor(id);
        if (wt.subscribers++ == 0) startObserving(wt);
        Stream s;
        s.id = id;
        s.live = true;
        ecs_iter_t it = ecs_each_id(m_world, id);
        while (ecs_each_next(&it))
            for (int32_t i = 0; i < it.count; ++i) push(s, it.entities[i], Kind::Added);
        m_streams.push_back(std::move(s));
        const StreamId sid = (StreamId)m_streams.size();
        wt.streams.push_back(sid);
        return sid;
    }

    // Ends a stream; its pending changes are dropped. Safe to call twice.
    void unsubscribe(StreamId sid) {
        Stream* s = get(sid);
        if (!s) return;
        Watch& wt = watchFor(s->id);
        for (size_t i = 0; i < wt.streams.size(); ++i)
            if (wt.streams[i] == sid) { wt.streams.erase(wt.streams.begin() + (std::ptrdiff_t)i); break; }
        *s = Stream{};
        if (--wt.subscribers == 0) stopObserving(wt);
    }

    // Up to `cap` changes, oldest first. Call until it returns fewer than
    // `cap`. Each returned entity is forgotten: a later change is new.
    uint32_t read(StreamId sid, Change* out, uint32_t cap) {
        Stream* s = get(sid);
        if (!s) return 0;
        uint32_t n = 0;
        while (s->head < s->q.size() && n < cap) {
            const Entry& e = s->q[s->head++];
            if (e.gone) continue;
            s->at.erase(e.change.entity);
            out[n++] = e.change;
        }
        if (s->head == s->q.size()) { s->q.clear(); s->head = 0; }
        return n;
    }

    // Changes waiting in a stream (merged, so at most one per entity).
    size_t pending(StreamId sid) const {
        const Stream* s = get(sid);
        return s ? s->at.size() : 0;
    }

    // Whether anything subscribes to `id` now. A writer asks once per pass.
    bool watching(flecs::id_t id) const {
        for (const auto& wt : m_watches) if (wt->id == id) return wt->subscribers != 0;
        return false;
    }

    // A declared write: `e`'s `id` changed in place.
    void changed(flecs::id_t id, flecs::entity_t e) {
        for (const auto& wt : m_watches)
            if (wt->id == id) { record(*wt, e, Kind::Changed); return; }
    }
    void changed(flecs::id_t id, const flecs::entity_t* es, uint32_t n) {
        for (const auto& wt : m_watches)
            if (wt->id == id) { for (uint32_t i = 0; i < n; ++i) record(*wt, es[i], Kind::Changed); return; }
    }

private:
    explicit Streams(flecs::world_t* w) : m_world(w), m_epoch(++s_epochs) {}

    struct Entry { Change change; bool gone = false; };
    struct Stream {
        flecs::id_t id = 0;
        bool        live = false;
        std::vector<Entry> q;     // in arrival order; [head, end) unread
        size_t      head = 0;
        std::unordered_map<flecs::entity_t, uint32_t> at;   // entity -> its entry in q
    };
    // One per id ever watched. Never freed while the world lives: a deferred
    // event may still reach an observer that is being deleted, and its ctx
    // must stay valid. Bounded by the number of distinct ids watched.
    struct Watch {
        Streams*        owner = nullptr;
        flecs::id_t     id = 0;
        uint32_t        subscribers = 0;
        flecs::entity_t observer = 0;
        std::vector<StreamId> streams;
    };

    Stream* get(StreamId sid) {
        return sid && sid <= m_streams.size() && m_streams[sid - 1].live ? &m_streams[sid - 1] : nullptr;
    }
    const Stream* get(StreamId sid) const {
        return sid && sid <= m_streams.size() && m_streams[sid - 1].live ? &m_streams[sid - 1] : nullptr;
    }

    Watch& watchFor(flecs::id_t id) {
        for (auto& wt : m_watches) if (wt->id == id) return *wt;
        m_watches.push_back(std::make_unique<Watch>());
        m_watches.back()->owner = this;
        m_watches.back()->id = id;
        return *m_watches.back();
    }

    // The merge rules in this file's header.
    static void push(Stream& s, flecs::entity_t e, Kind k) {
        auto f = s.at.find(e);
        if (f == s.at.end()) {
            s.at.emplace(e, (uint32_t)s.q.size());
            s.q.push_back({{e, k}, false});
            return;
        }
        Entry& en = s.q[f->second];
        switch (en.change.kind) {
        case Kind::Added:
            if (k == Kind::Removed) { en.gone = true; s.at.erase(f); }   // never seen: nothing
            break;                                                      // added then changed: ADDED
        case Kind::Changed:
            if (k == Kind::Removed) en.change.kind = Kind::Removed;
            break;
        case Kind::Removed:
            if (k == Kind::Added) en.change.kind = Kind::Changed;       // existed then, exists now
            break;
        }
    }

    void record(Watch& wt, flecs::entity_t e, Kind k) {
        for (StreamId sid : wt.streams)
            if (Stream* s = get(sid)) push(*s, e, k);
    }

    static void onEvent(ecs_iter_t* it) {
        Watch& wt = *static_cast<Watch*>(it->ctx);
        if (ecs_is_fini(it->real_world) || wt.subscribers == 0) return;
        const Kind k = it->event == EcsOnAdd ? Kind::Added
                     : it->event == EcsOnRemove ? Kind::Removed : Kind::Changed;
        for (int32_t i = 0; i < it->count; ++i) wt.owner->record(wt, it->entities[i], k);
    }

    void startObserving(Watch& wt) {
        ecs_observer_desc_t d = {};
        d.query.terms[0].id = wt.id;
        // Prefabs and disabled entities too: a provider decides what it
        // ignores, and an entity re-enabled must not come back unseen.
        d.query.flags = EcsQueryMatchPrefab | EcsQueryMatchDisabled;
        d.events[0] = EcsOnAdd;
        d.events[1] = EcsOnRemove;
        // OnSet only for an id with data: a tag or a wildcard pair has none.
        if (ecs_id_is_wildcard(wt.id) == false && ecs_get_type_info(m_world, wt.id))
            d.events[2] = EcsOnSet;
        d.callback = &Streams::onEvent;
        d.ctx = &wt;
        wt.observer = ecs_observer_init(m_world, &d);
    }
    void stopObserving(Watch& wt) {
        if (wt.observer && !ecs_is_fini(m_world)) ecs_delete(m_world, wt.observer);
        wt.observer = 0;
    }

    flecs::world_t* m_world;
    uint64_t        m_epoch;
    std::vector<Stream> m_streams;               // StreamId - 1
    std::vector<std::unique_ptr<Watch>> m_watches;
    static inline std::atomic<uint64_t> s_epochs{0};
};

// A writer's handle for one component, made once per pass outside its loop.
// Off (every call a no-op) when nothing subscribes to T in this world.
template <typename T>
class Marks {
public:
    explicit Marks(flecs::world_t* w) : m_s(Streams::of(w)) {
        if (m_s) {
            m_id = flecs::_::type<T>::id(w);
            if (!m_s->watching(m_id)) m_s = nullptr;
        }
    }
    explicit Marks(const flecs::world& w) : Marks(w.c_ptr()) {}
    bool on() const { return m_s != nullptr; }

    void entity(flecs::entity_t e) const { if (m_s) m_s->changed(m_id, e); }
    // Every row of the batch: the write declared ENG_ACCESS_WRITE.
    void column(const flecs::iter& it) const {
        if (m_s) m_s->changed(m_id, &it.entities()[0], (uint32_t)it.count());
    }
    // Rows [first, first + count) of the batch: ENG_ACCESS_WRITE_MARKED.
    void rows(const flecs::iter& it, uint32_t first, uint32_t count) const {
        if (m_s) m_s->changed(m_id, &it.entities()[0] + first, count);
    }

private:
    Streams*    m_s = nullptr;
    flecs::id_t m_id = 0;
};

// One entity, one component, written in place outside any pass.
template <typename T>
inline void markChanged(flecs::entity e) { Marks<T>(e.world().c_ptr()).entity(e.id()); }

} // namespace changes
