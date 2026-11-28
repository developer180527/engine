// ── eng_world — the world through the C API, over flecs (WO-073) ─────────────
//
// include/eng/eng_world.h is the contract; this is its implementation over the
// engine's flecs world. flecs is named here and nowhere in the header.
//
//   EngWorld      the engine's simulation world (one in this version)
//   EngEntity     the flecs id itself: it carries a generation, so a stale one
//                 is refused (ecs_is_alive) and never aliases a new entity
//   EngComponent  an index into the world's component table, which lives in a
//                 world singleton (CapiComponents) so it, and the sim hash
//                 context it hands out, last exactly as long as the world
//   EngQuery      an index and a generation into this handle's query table
//   EngChangeStream  a components/change_stream.h stream id
//   EngBlob       an index and a generation into the world's BlobStore, which
//                 a world singleton (DynamicBlobsRef) keeps alive
//   EngSystem     an index and a generation into this handle's system table;
//                 the phases run from the plugin registry's two edges
//
// Every entry point catches every exception: none may cross a C boundary.
#include <eng/engine.h>
#include <eng/eng_world.h>

#include "capi/eng_internal.h"
#include "components/change_stream.h"
#include "components/entity_id_util.h"
#include "components/name.h"
#include "components/transform_hierarchy.h"
#include "core/transform.h"
#include "core/logger.h"
#include "runtime/plugin.h"
#include "runtime/plugin_registry.h"
#include "runtime/runtime.h"
#include "runtime/sim_hash.h"
#include "scene/dynamic_schema.h"
#include "scene/entity_serializer.h"

#include <flecs.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

static_assert(sizeof(EngTransform) == sizeof(Transform), "EngTransform is core.Transform's bytes");
static_assert(sizeof(EngEntity) == sizeof(flecs::entity_t), "an EngEntity is a flecs id");

namespace {

EngResult fail(EngResult code, std::string msg) {
    engInternalSetError(std::move(msg));
    return code;
}
template <class R, class F>
R guard(R onError, F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        engInternalSetError("out of memory inside the engine");
    } catch (const std::exception& e) {
        engInternalSetError(std::string("internal error: ") + e.what());
    } catch (...) {
        engInternalSetError("internal error: an exception reached the C boundary");
    }
    return onError;
}

constexpr uint32_t kMaxTerms = 16;

// One component the C API can name. Engine ones ("core.Transform") have no
// field list here: their schema is the engine's own.
struct CompEntry {
    std::string name;
    flecs::entity_t id = 0;
    uint32_t size = 0, align = 0, revision = 0;
    bool engine = false;
    std::vector<EntitySerde::DynamicField> fields;
    std::vector<uint8_t> defaults;   // size bytes
    std::vector<uint32_t> blobOffsets;   // the BLOB fields' offsets, in field order
    class BlobStore* blobs = nullptr;    // the world's, when blobOffsets is not empty
};

// ── The blob store ───────────────────────────────────────────────────────────
// Every blob of one world. A handle is (generation << 32) | (slot + 1); a freed
// slot's generation moves on, so an old handle is refused and never aliases a
// new blob. A slot whose generation would wrap is retired, never reused.
//
// Ownership is per field: `owners` maps (entity, component) to the handles its
// BLOB fields hold, in field order. reconcile() is the one place it changes:
// it runs on OnSet (every engAddComponent, engSetComponent and scene load),
// and on every row a query WRITE column handed out. OnRemove frees what the
// removed component held.
constexpr uint32_t kMaxBlobBytes = 16u << 20;

class BlobStore final : public EntitySerde::DynamicBlobStore {
public:
    struct Slot {
        std::vector<uint8_t> bytes;
        uint32_t gen = 0;
        bool live = false, held = false;
        flecs::entity_t entity = 0, component = 0;   // the holder, when held
    };

    bool bytes(uint64_t h, const uint8_t*& p, uint32_t& n) const override {
        const Slot* s = find(h);
        if (!s) return false;
        p = s->bytes.empty() ? nullptr : s->bytes.data();
        n = (uint32_t)s->bytes.size();
        return true;
    }
    uint64_t create(const uint8_t* p, uint32_t n) override {
        if (n > kMaxBlobBytes) return 0;
        uint32_t i;
        if (!m_free.empty()) { i = m_free.back(); m_free.pop_back(); }
        else { i = (uint32_t)m_slots.size(); m_slots.emplace_back(); }
        Slot& s = m_slots[i];
        s.gen += 1;
        s.live = true;
        s.held = false;
        s.entity = s.component = 0;
        s.bytes.assign(p, p + (p ? n : 0));
        if (!p) s.bytes.assign(n, 0);
        ++m_live;
        return (uint64_t)s.gen << 32 | (uint64_t)(i + 1);
    }
    const Slot* find(uint64_t h) const {
        const uint32_t i = (uint32_t)h, gen = (uint32_t)(h >> 32);
        if (!i || i > m_slots.size()) return nullptr;
        const Slot& s = m_slots[i - 1];
        return s.live && s.gen == gen ? &s : nullptr;
    }
    Slot* find(uint64_t h) { return const_cast<Slot*>(static_cast<const BlobStore*>(this)->find(h)); }
    void free(uint64_t h) {
        Slot* s = find(h);
        if (!s) return;
        s->live = false;
        s->held = false;
        std::vector<uint8_t>().swap(s->bytes);
        --m_live;
        if (s->gen != 0xFFFFFFFFu) m_free.push_back((uint32_t)(h & 0xFFFFFFFFu) - 1);
    }
    size_t live() const { return m_live; }

    // The handles in the BLOB fields at `offsets` of (e, c)'s bytes become
    // what (e, c) holds. A handle it held and still holds stays; one nobody
    // holds is taken; one another field holds is copied, and the copy's
    // handle written into the field; anything else (0, freed, garbage) leaves
    // the field 0. What it held and no longer holds is freed.
    void reconcile(flecs::entity_t e, flecs::entity_t c, const std::vector<uint32_t>& offsets, uint8_t* bytes) {
        const Key k{e, c};
        auto found = m_owners.find(k);
        std::vector<uint64_t> old = found != m_owners.end() ? found->second : std::vector<uint64_t>();
        old.resize(offsets.size(), 0);
        std::vector<bool> kept(offsets.size(), false);
        std::vector<uint64_t> now(offsets.size(), 0);
        bool any = false;
        for (size_t f = 0; f < offsets.size(); ++f) {
            uint64_t h;
            std::memcpy(&h, bytes + offsets[f], 8);
            if (!h) continue;
            size_t j = 0;
            while (j < old.size() && !(old[j] == h && !kept[j])) ++j;
            if (j < old.size()) { kept[j] = true; now[f] = h; any = true; continue; }
            Slot* s = find(h);
            uint64_t mine = 0;
            if (s && !s->held) mine = h;
            else if (s) {
                const std::vector<uint8_t> copy = s->bytes;   // create() may move the slots
                mine = create(copy.data(), (uint32_t)copy.size());
            }
            if (Slot* m = mine ? find(mine) : nullptr) { m->held = true; m->entity = e; m->component = c; any = true; }
            std::memcpy(bytes + offsets[f], &mine, 8);
            now[f] = mine;
        }
        for (size_t j = 0; j < old.size(); ++j)
            if (old[j] && !kept[j]) free(old[j]);
        if (any) m_owners[k] = std::move(now);
        else if (found != m_owners.end()) m_owners.erase(k);
    }
    void released(flecs::entity_t e, flecs::entity_t c) {
        auto found = m_owners.find(Key{e, c});
        if (found == m_owners.end()) return;
        for (uint64_t h : found->second) free(h);
        m_owners.erase(found);
    }

private:
    struct Key {
        flecs::entity_t e, c;
        bool operator==(const Key& o) const { return e == o.e && c == o.c; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return std::hash<uint64_t>()(k.e * 0x9E3779B97F4A7C15ull ^ k.c); }
    };
    std::vector<Slot> m_slots;
    std::vector<uint32_t> m_free;
    std::unordered_map<Key, std::vector<uint64_t>, KeyHash> m_owners;
    size_t m_live = 0;
};

// The world's store, made on first use. A singleton keeps it, and each blob
// observer keeps it too, so it outlives every OnRemove of the world's end.
std::shared_ptr<BlobStore> blobsOf(flecs::world& w) {
    if (const EntitySerde::DynamicBlobsRef* r = w.try_get<EntitySerde::DynamicBlobsRef>())
        return std::static_pointer_cast<BlobStore>(r->store);
    simhash::exemptId(w, w.component<EntitySerde::DynamicBlobsRef>(),
                      "C API bookkeeping — where blob bytes live; each blob is hashed through the field that holds it");
    auto store = std::make_shared<BlobStore>();
    w.set<EntitySerde::DynamicBlobsRef>({store});
    return store;
}

struct BlobWatch {
    std::shared_ptr<BlobStore> store;
    flecs::entity_t id;
    std::vector<uint32_t> offsets;
};
void onBlobEvent(ecs_iter_t* it) {
    if (ecs_is_fini(it->real_world)) return;   // the store goes with the world
    BlobWatch& bw = *static_cast<BlobWatch*>(it->ctx);
    if (it->event == EcsOnRemove) {
        for (int32_t i = 0; i < it->count; ++i) bw.store->released(it->entities[i], bw.id);
        return;
    }
    uint8_t* col = static_cast<uint8_t*>(ecs_field_w_size(it, (size_t)ecs_get_type_info(it->world, bw.id)->size, 0));
    const size_t stride = (size_t)ecs_get_type_info(it->world, bw.id)->size;
    for (int32_t i = 0; i < it->count; ++i)
        bw.store->reconcile(it->entities[i], bw.id, bw.offsets, col + (size_t)i * stride);
}
void watchBlobs(flecs::world& w, const CompEntry& c) {
    ecs_observer_desc_t d = {};
    d.query.terms[0].id = c.id;
    d.query.terms[0].src.id = EcsSelf;
    d.query.flags = EcsQueryMatchPrefab | EcsQueryMatchDisabled;
    d.events[0] = EcsOnSet;
    d.events[1] = EcsOnRemove;
    d.callback = &onBlobEvent;
    d.ctx = new BlobWatch{blobsOf(w), c.id, c.blobOffsets};
    d.ctx_free = [](void* p) { delete static_cast<BlobWatch*>(p); };
    ecs_observer_init(w.c_ptr(), &d);
}

// The world's table of C API components, as a singleton of that world: the
// sim hash keeps a pointer into it (std::deque never moves an element).
struct CapiComponents {
    std::deque<CompEntry> comps;
};

// The sim hash of a C component: each field by type, so padding never counts.
void hashFields(flecs::entity, const void* p, const void* ctx, simhash::Digest& d) {
    const CompEntry& c = *static_cast<const CompEntry*>(ctx);
    const uint8_t* b = static_cast<const uint8_t*>(p);
    namespace dyn = EntitySerde::dyn;
    for (const EntitySerde::DynamicField& f : c.fields) {
        const uint8_t* q = b + f.offset;
        if (f.type == dyn::FixedString) {
            d.str(std::string_view(reinterpret_cast<const char*>(q), strnlen(reinterpret_cast<const char*>(q), f.count)));
            continue;
        }
        if (f.type == dyn::Blob) {   // its bytes, never its handle
            uint64_t h; std::memcpy(&h, q, 8);
            const uint8_t* bp = nullptr; uint32_t bn = 0;
            if (!h || !c.blobs || !c.blobs->bytes(h, bp, bn)) { d.u64(0); d.u64(0); continue; }
            d.u64(1); d.u64(bn);
            if (bn) d.bytes(bp, bn);
            continue;
        }
        const uint32_t n = f.count ? f.count : 1, vs = dyn::valueSize(f.type);
        for (uint32_t i = 0; i < n; ++i, q += vs) {
            if (const uint32_t fl = dyn::floatsOf(f.type)) {
                for (uint32_t k = 0; k < fl; ++k) { float v; std::memcpy(&v, q + 4 * k, 4); d.f32(v); }
            } else if (f.type == dyn::F64) {
                double v; std::memcpy(&v, q, 8); d.f64(v);
            } else {
                uint64_t v = 0; std::memcpy(&v, q, vs); d.u64(v);
            }
        }
    }
}

struct QueryEntry {
    ecs_query_t* q = nullptr;
    uint16_t gen = 0;
    uint32_t open = 0;                    // iterations between Begin and End
    std::vector<EngAccess> access;        // per term
    std::vector<flecs::entity_t> ids;     // per term
    std::vector<uint32_t> sizes;          // per term
    std::vector<const CompEntry*> blobTerms;  // per term: the component when a WRITE column holds blobs
    std::vector<uint32_t> users;          // systems that run it (indices)
    const void* owner = nullptr;          // the C kit whose callback made it, or null
};

// ── Systems ──
struct SystemEntry {
    std::string name;
    EngPhase phase = 0;
    EngQuery query = 0;
    EngSystemFn fn = nullptr;
    void* user = nullptr;
    std::vector<std::string> after;
    uint64_t seq = 0;      // creation order
    uint16_t gen = 0;
    bool live = false;
    const void* owner = nullptr;   // the C kit whose callback made it, or null
};
constexpr uint32_t kPhases = 3;

} // namespace

struct EngWorld {
    // Who holds it: the program (engCreateWorld; engine set), and the C kits
    // a project loaded (kitHolders). One world per simulation world, gone
    // when the last lets go.
    EngEngine*      engine = nullptr;
    EngineRuntime*  rt = nullptr;
    uint32_t        kitHolders = 0;
    flecs::world*   w = nullptr;
    CapiComponents* table = nullptr;
    std::vector<QueryEntry> queries;
    std::vector<changes::StreamId> streams;
    uint32_t openIters = 0;
    // Systems. A deque: creating one from inside a system moves nothing.
    std::deque<SystemEntry> systems;
    std::vector<uint32_t> order[kPhases];   // indices, in run order
    bool orderDirty[kPhases] = {false, false, false};
    uint64_t nextSeq = 1;
    uint32_t running = 0;                   // phases running right now
    std::unique_ptr<IEnginePlugin> first, last;   // the plugin registry's edges
    // The C kit whose code runs right now (its callback, or a system it made):
    // what is created is its, and goes when it stops (engInternalDropOwned).
    const void* owner = nullptr;
};

struct EngQueryIter {
    EngWorld* world = nullptr;
    uint32_t query = 0;              // index
    ecs_iter_t it{};
    bool started = false, done = false, batch = false;
    std::vector<void*> cols;
};

namespace {

flecs::world_t* raw(EngWorld* w) { return w->w->c_ptr(); }

EngResult checkWorld(EngWorld* w) {
    if (!w) return fail(ENG_ERROR_INVALID_ARGUMENT, "world is NULL");
    return ENG_OK;
}
EngResult checkEntity(EngWorld* w, EngEntity e) {
    if (EngResult r = checkWorld(w); r != ENG_OK) return r;
    if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "entity is 0");
    if (!ecs_is_valid(raw(w), e) || !ecs_is_alive(raw(w), e))
        return fail(ENG_ERROR_STALE_HANDLE, "entity " + std::to_string(e) + " has been destroyed");
    return ENG_OK;
}
const CompEntry* comp(EngWorld* w, EngComponent c) {
    if (!c || c > w->table->comps.size()) return nullptr;
    return &w->table->comps[c - 1];
}
EngResult checkComp(EngWorld* w, EngComponent c, const CompEntry** out) {
    *out = comp(w, c);
    if (!*out) return fail(ENG_ERROR_INVALID_ARGUMENT, "component " + std::to_string(c) +
                                                       " is not registered in this world");
    return ENG_OK;
}
EngResult checkSize(const CompEntry& c, uint32_t size) {
    if (size != c.size)
        return fail(ENG_ERROR_INVALID_ARGUMENT, c.name + " is " + std::to_string(c.size) +
                                                " bytes; size was " + std::to_string(size));
    return ENG_OK;
}

CapiComponents* tableOf(flecs::world& w) {
    if (!w.has<CapiComponents>()) {
        simhash::exemptId(w, w.component<CapiComponents>(),
                          "C API bookkeeping — which component ids the C API names; never simulated");
        w.set<CapiComponents>({});
        // The engine components this version exposes as bytes.
        CompEntry t;
        t.name = "core.Transform";
        t.id = w.id<Transform>();
        t.size = sizeof(Transform);
        t.align = alignof(Transform);
        t.engine = true;
        w.get_mut<CapiComponents>().comps.push_back(std::move(t));
    }
    return &w.get_mut<CapiComponents>();
}

// A field list is checked as a whole: known types, inside the component,
// no two fields sharing a byte, names present and distinct.
EngResult checkFields(const EngComponentDesc& d, std::vector<EntitySerde::DynamicField>& out) {
    namespace dyn = EntitySerde::dyn;
    if (d.field_count && !d.fields) return fail(ENG_ERROR_INVALID_ARGUMENT, "field_count is not zero but fields is NULL");
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (uint32_t i = 0; i < d.field_count; ++i) {
        const EngFieldDesc& f = d.fields[i];
        const std::string where = std::string(d.name) + " field " + std::to_string(i);
        if (!f.name || !*f.name) return fail(ENG_ERROR_INVALID_ARGUMENT, where + " has no name");
        if (f.reserved) return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "'): reserved must be zero");
        if (f.type == ENG_FIELD_ASSET)
            return fail(ENG_ERROR_UNSUPPORTED, where + " ('" + f.name + "'): ASSET fields are not in this version");
        if (f.type == ENG_FIELD_BLOB && f.count > 1)
            return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "'): a BLOB field is one handle (count 0 or 1)");
        const uint32_t vs = dyn::valueSize(f.type);
        if (!vs || f.type > ENG_FIELD_BLOB)
            return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "'): unknown field type " + std::to_string(f.type));
        const uint32_t count = f.count ? f.count : 1;
        if (f.type == ENG_FIELD_FIXED_STRING && count < 2)
            return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "'): a FIXED_STRING needs count >= 2 (bytes, NUL included)");
        const uint64_t end = (uint64_t)f.offset + (uint64_t)vs * count;
        if (end > d.size)
            return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "') ends at byte " + std::to_string(end) +
                        ", past the component's " + std::to_string(d.size));
        for (const auto& [a, b] : spans)
            if (f.offset < b && end > a)
                return fail(ENG_ERROR_INVALID_ARGUMENT, where + " ('" + f.name + "') overlaps another field");
        for (const auto& o : out)
            if (o.name == f.name) return fail(ENG_ERROR_INVALID_ARGUMENT, where + ": '" + f.name + "' is named twice");
        spans.emplace_back(f.offset, (uint32_t)end);
        out.push_back({f.name, f.type, f.offset, count});
    }
    return ENG_OK;
}

bool sameSchema(const CompEntry& c, const EngComponentDesc& d, const std::vector<EntitySerde::DynamicField>& fields,
                const std::vector<uint8_t>& defaults, std::string& why) {
    if (c.engine) { why = "it is the engine's own"; return false; }
    if (c.size != d.size || c.align != d.align) {
        why = "registered as " + std::to_string(c.size) + " bytes aligned " + std::to_string(c.align) +
              ", now " + std::to_string(d.size) + " aligned " + std::to_string(d.align);
        return false;
    }
    if (c.revision != d.revision) {
        why = "registered at revision " + std::to_string(c.revision) + ", now " + std::to_string(d.revision);
        return false;
    }
    if (c.fields.size() != fields.size()) {
        why = "registered with " + std::to_string(c.fields.size()) + " fields, now " + std::to_string(fields.size());
        return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& a = c.fields[i];
        const auto& b = fields[i];
        if (a.name != b.name || a.type != b.type || a.offset != b.offset || a.count != b.count) {
            why = "field " + std::to_string(i) + " was '" + a.name + "' (type " + std::to_string(a.type) +
                  ", offset " + std::to_string(a.offset) + ", count " + std::to_string(a.count) + "), now '" +
                  b.name + "' (type " + std::to_string(b.type) + ", offset " + std::to_string(b.offset) +
                  ", count " + std::to_string(b.count) + ")";
            return false;
        }
    }
    if (c.defaults != defaults) { why = "its defaults differ"; return false; }
    return true;
}

QueryEntry* queryOf(EngWorld* w, EngQuery q) {
    const uint32_t i = q & 0xFFFFu, gen = q >> 16;
    if (!q || i == 0 || i > w->queries.size()) return nullptr;
    QueryEntry& e = w->queries[i - 1];
    return (e.q && e.gen == gen) ? &e : nullptr;
}

void markRows(EngWorld* w, flecs::entity_t id, const EngEntity* es, uint32_t n) {
    if (changes::Streams* s = changes::Streams::of(raw(w)); s && s->watching(id))
        s->changed(id, es, n);
}

// The batch a query handed out is done with: every row of a WRITE column
// holding blob fields is reconciled, as a set would be (no OnSet fires for a
// write in place). Costs nothing for a query that writes no blob component.
void settleBatch(EngQueryIter* it) {
    if (!it->batch) return;
    it->batch = false;
    const QueryEntry& qe = it->world->queries[it->query - 1];
    for (size_t i = 0; i < qe.blobTerms.size(); ++i) {
        const CompEntry* c = qe.blobTerms[i];
        if (!c || !it->cols[i]) continue;
        uint8_t* col = static_cast<uint8_t*>(it->cols[i]);
        for (int32_t r = 0; r < it->it.count; ++r)
            c->blobs->reconcile(it->it.entities[r], c->id, c->blobOffsets, col + (size_t)r * c->size);
    }
}

// ── Systems: order ──
// The live systems of `phase`, plus `extra` (index into w->systems, or -1),
// sorted by creation order and moved only as run_after requires: of the
// systems whose predecessors have all run, the earliest created goes next.
// A name not created yet has no edge. On a cycle, `cycle` names it and the
// result is empty.
std::vector<uint32_t> sortPhase(EngWorld* w, EngPhase phase, int64_t extra, std::string* cycle) {
    std::vector<uint32_t> nodes;
    for (uint32_t i = 0; i < w->systems.size(); ++i)
        if ((w->systems[i].live || (int64_t)i == extra) && w->systems[i].phase == phase) nodes.push_back(i);
    std::sort(nodes.begin(), nodes.end(), [&](uint32_t a, uint32_t b) { return w->systems[a].seq < w->systems[b].seq; });
    const size_t n = nodes.size();
    std::vector<std::vector<size_t>> preds(n);   // positions in nodes
    for (size_t a = 0; a < n; ++a)
        for (const std::string& name : w->systems[nodes[a]].after)
            for (size_t b = 0; b < n; ++b)
                if (w->systems[nodes[b]].name == name) preds[a].push_back(b);
    std::vector<bool> done(n, false);
    std::vector<uint32_t> out;
    out.reserve(n);
    for (size_t round = 0; round < n; ++round) {
        size_t pick = n;
        for (size_t a = 0; a < n && pick == n; ++a) {
            if (done[a]) continue;
            bool ready = true;
            for (size_t b : preds[a]) ready = ready && done[b];
            if (ready) pick = a;
        }
        if (pick == n) break;
        done[pick] = true;
        out.push_back(nodes[pick]);
    }
    if (out.size() == n) return out;
    if (cycle) {
        // Walk predecessors among what is left until a system repeats.
        size_t at = 0;
        while (done[at]) ++at;
        std::vector<size_t> path;
        std::vector<int> seen(n, -1);
        while (seen[at] < 0) {
            seen[at] = (int)path.size();
            path.push_back(at);
            for (size_t b : preds[at]) if (!done[b]) { at = b; break; }
        }
        std::string text;
        for (size_t k = path.size(); k-- > (size_t)seen[at];) text += w->systems[nodes[path[k]]].name + " -> ";
        *cycle = text + w->systems[nodes[path.back()]].name;
    }
    return {};
}

// ── Systems: running a phase ──
void runPhase(EngWorld* w, EngPhase phase, double dt) {
    if (w->orderDirty[phase]) {
        w->order[phase] = sortPhase(w, phase, -1, nullptr);
        w->orderDirty[phase] = false;
    }
    if (w->order[phase].empty()) return;
    const std::vector<uint32_t> order = w->order[phase];   // a system may create or destroy systems
    EngineRuntime* rt = w->rt;
    ++w->running;
    for (uint32_t idx : order) {
        const SystemEntry& s = w->systems[idx];
        if (!s.live) continue;
        const uint16_t gen = s.gen;
        auto stillLive = [&] { return w->systems[idx].live && w->systems[idx].gen == gen; };
        EngSystemContext ctx{};
        ctx.struct_size = sizeof ctx;
        ctx.delta_seconds = dt;
        ctx.tick = rt ? rt->simulationTicks() : 0;
        ctx.world = w;
        ctx.user = s.user;
        const EngSystemFn fn = s.fn;
        struct OwnerScope {
            EngWorld* w; const void* was;
            OwnerScope(EngWorld* w_, const void* o) : w(w_), was(w_->owner) { w->owner = o; }
            ~OwnerScope() { w->owner = was; }
        } scope(w, s.owner);
        if (!s.query) { fn(&ctx, nullptr); continue; }
        EngQueryIter* it = nullptr;
        if (engQueryBegin(w, s.query, &it) != ENG_OK) {
            LOG_ERROR("EngWorld", "system '%s' could not run its query: %s", s.name.c_str(), engGetLastErrorMessage());
            continue;
        }
        ctx.iter = it;
        EngQueryBatch b{};
        b.struct_size = sizeof b;
        while (stillLive() && engQueryNext(it, &b)) fn(&ctx, &b);
        engQueryEnd(it);
    }
    --w->running;
}

// The plugin registry's edges: the first runs FIXED_UPDATE (each step) and
// UPDATE (each frame) before every kit; the last runs LATE_UPDATE after all.
class PhaseEdge final : public IEnginePlugin {
public:
    PhaseEdge(EngWorld* w, bool first) : m_w(w), m_first(first) {}
    const char* name() const override { return m_first ? "eng.world (first edge)" : "eng.world (last edge)"; }
    const char* version() const override { return "1"; }
    void onAttach(RuntimeContext&) override {}
    void onDetach() override {}
    void onSimulationStart(flecs::world&) override {}
    void onSimulationStop() override {}
    void onUpdate(flecs::world& gw, float) override {
        if (m_first && gw.c_ptr() == raw(m_w)) runPhase(m_w, ENG_PHASE_FIXED_UPDATE, 1.0 / 60.0);   // kSimDt, exactly
    }
    void onFrame(flecs::world& gw, float dt) override {
        if (gw.c_ptr() == raw(m_w)) runPhase(m_w, m_first ? ENG_PHASE_UPDATE : ENG_PHASE_LATE_UPDATE, dt);
    }
private:
    EngWorld* m_w;
    bool m_first;
};

EngSystem systemHandle(const EngWorld* w, uint32_t i) { return ((EngSystem)w->systems[i].gen << 16) | (EngSystem)(i + 1); }
SystemEntry* systemOf(EngWorld* w, EngSystem h, uint32_t* index = nullptr) {
    const uint32_t i = h & 0xFFFFu, gen = h >> 16;
    if (!i || i > w->systems.size()) return nullptr;
    SystemEntry& s = w->systems[i - 1];
    if (!s.live || s.gen != gen) return nullptr;
    if (index) *index = i - 1;
    return &s;
}
BlobStore* storeOf(EngWorld* w) {
    const EntitySerde::DynamicBlobsRef* r = w->w->try_get<EntitySerde::DynamicBlobsRef>();
    return r ? static_cast<BlobStore*>(r->store.get()) : nullptr;
}

} // namespace

uint64_t engInternalComponentId(EngWorld* w, EngComponent c) {
    const CompEntry* ce = w ? comp(w, c) : nullptr;
    return ce ? ce->id : 0;
}

namespace {
// The C world of each simulation world, while someone holds it.
std::unordered_map<flecs::world_t*, EngWorld*>& worlds() {
    static std::unordered_map<flecs::world_t*, EngWorld*> m;
    return m;
}

EngWorld* makeWorld(EngineRuntime* rt, flecs::world& fw) {
    auto w = std::make_unique<EngWorld>();
    w->rt = rt;
    w->w = &fw;
    w->table = tableOf(fw);
    w->first = std::make_unique<PhaseEdge>(w.get(), true);
    w->last = std::make_unique<PhaseEdge>(w.get(), false);
    rt->plugins().setEdges(w->first.get(), w->last.get());
    worlds()[fw.c_ptr()] = w.get();
    return w.release();
}

void destroyIfUnheld(EngWorld* w) {
    if (w->engine || w->kitHolders) return;
    w->rt->plugins().setEdges(nullptr, nullptr);
    for (QueryEntry& q : w->queries)
        if (q.q) ecs_query_fini(q.q);
    if (changes::Streams* s = changes::Streams::of(raw(w)))
        for (changes::StreamId id : w->streams) s->unsubscribe(id);
    worlds().erase(raw(w));
    delete w;
}
} // namespace

// The program lets go (engDestroyWorld, engDestroyEngine, a failed create).
void engInternalDestroyWorld(EngWorld* w) {
    if (!w) return;
    w->engine = nullptr;
    destroyIfUnheld(w);
}

EngWorld* engInternalAcquireKitWorld(EngineRuntime* rt, flecs::world& fw) {
    auto it = worlds().find(fw.c_ptr());
    EngWorld* w = it != worlds().end() ? it->second : makeWorld(rt, fw);
    ++w->kitHolders;
    return w;
}

void engInternalReleaseKitWorld(EngWorld* w) {
    if (!w || !w->kitHolders) return;
    --w->kitHolders;
    destroyIfUnheld(w);
}

bool engInternalWorldBusy(EngWorld* w) { return w && (w->running || w->openIters); }

const void* engInternalSetOwner(EngWorld* w, const void* owner) {
    const void* was = w->owner;
    w->owner = owner;
    return was;
}

uint32_t engInternalDropOwned(EngWorld* w, const void* owner) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->systems.size(); ++i) {
        SystemEntry& s = w->systems[i];
        if (!s.live || s.owner != owner) continue;
        s.live = false;
        w->orderDirty[s.phase] = true;
        if (QueryEntry* qe = queryOf(w, s.query))
            qe->users.erase(std::remove(qe->users.begin(), qe->users.end(), i), qe->users.end());
        ++n;
    }
    for (QueryEntry& q : w->queries)
        if (q.q && q.owner == owner && !q.open) { ecs_query_fini(q.q); q.q = nullptr; ++n; }
    return n;
}

extern "C" {

// ── The world ────────────────────────────────────────────────────────────────
EngResult engCreateWorld(EngEngine* engine, const EngWorldDesc* desc, EngWorld** out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = nullptr;
        if (!engine) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        if (desc) {
            if (desc->struct_size < sizeof(uint32_t)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc has no struct_size (start it with ENG_INIT)");
            EngWorldDesc d{};
            std::memcpy(&d, desc, std::min<size_t>(desc->struct_size, sizeof d));
            if (d.reserved) return fail(ENG_ERROR_INVALID_ARGUMENT, "reserved must be zero");
            // 1/60: EngineRuntime::kSimDt, the one step this version runs.
            if (d.fixed_step_seconds != 0.0 && std::fabs(d.fixed_step_seconds - 1.0 / 60.0) > 1e-9)
                return fail(ENG_ERROR_UNSUPPORTED, "this version runs one fixed step, 1/60 s; fixed_step_seconds was " +
                                                   std::to_string(d.fixed_step_seconds));
        }
        if (engInternalInModuleCall(engine))
            return fail(ENG_ERROR_REFUSED, "engCreateWorld refused: called from a module's callback");
        EngWorld*& slot = engInternalWorldSlot(engine);
        if (slot) return fail(ENG_ERROR_UNSUPPORTED, "this version has one world, and its handle is held "
                                                     "(engDestroyWorld it first)");
        EngineRuntime* rt = engInternalRuntime(engine);
        if (!rt) return ENG_ERROR_UNSUPPORTED;   // the message is the start's
        // The project's C kits may hold this simulation's world already: the
        // program shares it.
        auto it = worlds().find(rt->simWorld().c_ptr());
        EngWorld* w = it != worlds().end() ? it->second : makeWorld(rt, rt->simWorld());
        w->engine = engine;
        slot = w;
        // The modules' on_world_start, in order. One failing: the program's
        // hold is gone again and the slot free, as if the call had not happened.
        if (EngResult r = engInternalStartModules(engine, w); r != ENG_OK) {
            slot = nullptr;
            engInternalDestroyWorld(w);
            return r;
        }
        *out = w;
        return ENG_OK;
    });
}

void engDestroyWorld(EngWorld* w) {
    guard<int>(0, [&] {
        if (!w) return 0;
        if (w->openIters) {
            engInternalSetError("engDestroyWorld refused: a query iteration is open (engQueryEnd it first)");
            return 0;
        }
        if (w->running) {
            engInternalSetError("engDestroyWorld refused: called from inside a system");
            return 0;
        }
        if (!w->engine) {
            engInternalSetError("engDestroyWorld refused: this world is the project's simulation, "
                                "and only its program's engCreateWorld handle can be destroyed");
            return 0;
        }
        if (engInternalInModuleCall(w->engine)) {
            engInternalSetError("engDestroyWorld refused: called from a module's callback");
            return 0;
        }
        // The modules stop first, in reverse, while the world still works.
        engInternalStopModules(w->engine, w);
        engInternalWorldSlot(w->engine) = nullptr;
        engInternalDestroyWorld(w);
        return 0;
    });
}

// ── Entities ─────────────────────────────────────────────────────────────────
EngResult engCreateEntity(EngWorld* w, const char* name, EngEntity* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        flecs::entity e = w->w->entity();
        if (name) e.set<Name>({name});
        *out = e.id();
        return ENG_OK;
    });
}

void engDestroyEntity(EngWorld* w, EngEntity e) {
    guard<int>(0, [&] {
        if (checkEntity(w, e) != ENG_OK) return 0;
        ecs_delete(raw(w), e);
        return 0;
    });
}

EngBool engIsEntityAlive(EngWorld* w, EngEntity e) {
    return guard<EngBool>(ENG_FALSE, [&]() -> EngBool {
        return w && e && ecs_is_valid(raw(w), e) && ecs_is_alive(raw(w), e) ? ENG_TRUE : ENG_FALSE;
    });
}

EngEntity engFindEntity(EngWorld* w, const char* name) {
    return guard<EngEntity>(0, [&]() -> EngEntity {
        if (checkWorld(w) != ENG_OK || !name) return 0;
        EngEntity found = 0;
        w->w->each([&](flecs::entity e, const Name& n) {
            if (!found && n.value == name) found = e.id();
        });
        return found;
    });
}

EngResult engSetEntityParent(EngWorld* w, EngEntity child, EngEntity parent) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, child); r != ENG_OK) return r;
        flecs::entity c(*w->w, child);
        if (!parent) { c.remove(flecs::ChildOf, flecs::Wildcard); return ENG_OK; }
        if (EngResult r = checkEntity(w, parent); r != ENG_OK) return r;
        if (!safeReparent(c, flecs::entity(*w->w, parent)))
            return fail(ENG_ERROR_REFUSED, "parenting " + std::to_string(child) + " to " + std::to_string(parent) +
                                           " was refused: it would make a cycle, or a chain too deep");
        return ENG_OK;
    });
}

EngEntity engGetEntityParent(EngWorld* w, EngEntity child) {
    return guard<EngEntity>(0, [&]() -> EngEntity {
        if (checkEntity(w, child) != ENG_OK) return 0;
        return flecs::entity(*w->w, child).target(flecs::ChildOf).id();
    });
}

EngResult engGetEntityStableId(EngWorld* w, EngEntity e, uint64_t out[2]) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, e); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        out[0] = ensureEntityId(flecs::entity(*w->w, e));
        out[1] = 0;
        return ENG_OK;
    });
}

EngEntity engFindEntityByStableId(EngWorld* w, const uint64_t id[2]) {
    return guard<EngEntity>(0, [&]() -> EngEntity {
        if (checkWorld(w) != ENG_OK || !id || !id[0] || id[1]) return 0;
        return findById(*w->w, id[0]).id();
    });
}

// ── Components ───────────────────────────────────────────────────────────────
EngResult engRegisterComponent(EngWorld* w, const EngComponentDesc* in, EngComponent* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = 0;
        if (!in || in->struct_size < sizeof(uint32_t)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc is NULL or has no struct_size (start it with ENG_INIT)");
        EngComponentDesc d{};
        std::memcpy(&d, in, std::min<size_t>(in->struct_size, sizeof d));
        if (!d.name || !*d.name) return fail(ENG_ERROR_INVALID_ARGUMENT, "the component has no name");
        const std::string name = d.name;
        if (name.find('.') == std::string::npos || name.front() == '.' || name.back() == '.')
            return fail(ENG_ERROR_INVALID_ARGUMENT, "'" + name + "' is not namespaced (\"kit.Name\")");
        if (name.rfind("core.", 0) == 0)
            return fail(ENG_ERROR_REFUSED, "'" + name + "' is the engine's namespace: use engFindComponent");
        if (d.reserved0) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": reserved0 must be zero");
        if (d.migrate) return fail(ENG_ERROR_UNSUPPORTED, name + ": migrate is not in this version");
        if (!d.size) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": size is 0 (a tag is not a component here)");
        if (!d.align || (d.align & (d.align - 1)) || d.align > 64 || d.size % d.align)
            return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": align " + std::to_string(d.align) +
                                                    " is not a power of two up to 64 that divides size " + std::to_string(d.size));
        std::vector<EntitySerde::DynamicField> fields;
        if (EngResult r = checkFields(d, fields); r != ENG_OK) return r;
        std::vector<uint8_t> defaults(d.size, 0);
        if (d.defaults) std::memcpy(defaults.data(), d.defaults, d.size);
        std::vector<uint32_t> blobOffsets;
        for (const auto& f : fields) {
            if (f.type != ENG_FIELD_BLOB) continue;
            uint64_t h; std::memcpy(&h, defaults.data() + f.offset, 8);
            if (h) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": the default of BLOB field '" + f.name +
                                                           "' must be 0 (a default cannot hold bytes)");
            blobOffsets.push_back(f.offset);
        }

        for (size_t i = 0; i < w->table->comps.size(); ++i) {
            const CompEntry& c = w->table->comps[i];
            if (c.name != name) continue;
            std::string why;
            if (!sameSchema(c, d, fields, defaults, why))
                return fail(ENG_ERROR_REFUSED, "'" + name + "' is already registered differently: " + why);
            *out = (EngComponent)(i + 1);
            return ENG_OK;
        }
        if (const auto* s = EntitySerde::schemas().byName(name); s && s->key != name)
            return fail(ENG_ERROR_REFUSED, "'" + name + "' is already a scene schema of the engine's");

        // The flecs component: plain bytes, no hooks. Named for diagnostics
        // only; the C API finds it through this table, never by flecs path.
        ecs_entity_t id = ecs_new(raw(w));
        ecs_set_name(raw(w), id, name.c_str());
        ecs_component_desc_t cd{};
        cd.entity = id;
        cd.type.size = (ecs_size_t)d.size;
        cd.type.alignment = (ecs_size_t)d.align;
        ecs_component_init(raw(w), &cd);

        CompEntry c;
        c.name = name;
        c.id = id;
        c.size = d.size;
        c.align = d.align;
        c.revision = d.revision;
        c.fields = fields;
        c.defaults = defaults;
        c.blobOffsets = blobOffsets;
        if (!blobOffsets.empty()) c.blobs = blobsOf(*w->w).get();
        w->table->comps.push_back(std::move(c));
        const CompEntry& kept = w->table->comps.back();
        // Blob fields own their blobs: taken on every set, freed on remove.
        if (!kept.blobOffsets.empty()) watchBlobs(*w->w, kept);
        // Simulation state: a kit's component is gameplay unless it says
        // otherwise, and a hash over each field never reads padding.
        simhash::declareIdWithContext(*w->w, flecs::entity(*w->w, id), hashFields, &kept);
        // Saved and loaded with scenes, field by field, under its name. The
        // registry is the process's: an identical schema again is a no-op.
        if (!EntitySerde::schemas().add(EntitySerde::dynamicSchema(name, d.revision, id, d.size, fields, defaults)))
            return fail(ENG_ERROR_REFUSED, "'" + name + "' could not be added to the scene schemas (see the log)");
        *out = (EngComponent)w->table->comps.size();
        return ENG_OK;
    });
}

EngComponent engFindComponent(EngWorld* w, const char* name) {
    return guard<EngComponent>(0, [&]() -> EngComponent {
        if (checkWorld(w) != ENG_OK) return 0;
        if (!name) { engInternalSetError("name is NULL"); return 0; }
        for (size_t i = 0; i < w->table->comps.size(); ++i)
            if (w->table->comps[i].name == name) return (EngComponent)(i + 1);
        engInternalSetError(std::string("no component '") + name + "' in this world" +
                            (std::string(name).rfind("core.", 0) == 0
                                 ? " (this version exposes core.Transform as bytes)" : ""));
        return 0;
    });
}

// ── One entity at a time ─────────────────────────────────────────────────────
EngResult engAddComponent(EngWorld* w, EngEntity e, EngComponent c, const void* initial) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, e); r != ENG_OK) return r;
        const CompEntry* ce;
        if (EngResult r = checkComp(w, c, &ce); r != ENG_OK) return r;
        if (ecs_has_id(raw(w), e, ce->id)) return fail(ENG_ERROR_REFUSED, "entity already has " + ce->name);
        const void* bytes = initial;
        Transform identity;
        if (!bytes) bytes = ce->engine ? static_cast<const void*>(&identity) : ce->defaults.data();
        ecs_set_id(raw(w), e, ce->id, ce->size, bytes);
        return ENG_OK;
    });
}

EngResult engRemoveComponent(EngWorld* w, EngEntity e, EngComponent c) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, e); r != ENG_OK) return r;
        const CompEntry* ce;
        if (EngResult r = checkComp(w, c, &ce); r != ENG_OK) return r;
        ecs_remove_id(raw(w), e, ce->id);
        return ENG_OK;
    });
}

EngBool engHasComponent(EngWorld* w, EngEntity e, EngComponent c) {
    return guard<EngBool>(ENG_FALSE, [&]() -> EngBool {
        const CompEntry* ce;
        if (checkEntity(w, e) != ENG_OK || checkComp(w, c, &ce) != ENG_OK) return ENG_FALSE;
        return ecs_has_id(raw(w), e, ce->id) ? ENG_TRUE : ENG_FALSE;
    });
}

EngResult engGetComponent(EngWorld* w, EngEntity e, EngComponent c, void* out, uint32_t size) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, e); r != ENG_OK) return r;
        const CompEntry* ce;
        if (EngResult r = checkComp(w, c, &ce); r != ENG_OK) return r;
        if (EngResult r = checkSize(*ce, size); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        const void* p = ecs_get_id(raw(w), e, ce->id);
        if (!p) return fail(ENG_ERROR_NOT_FOUND, "entity has no " + ce->name);
        std::memcpy(out, p, size);
        return ENG_OK;
    });
}

EngResult engSetComponent(EngWorld* w, EngEntity e, EngComponent c, const void* bytes, uint32_t size) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkEntity(w, e); r != ENG_OK) return r;
        const CompEntry* ce;
        if (EngResult r = checkComp(w, c, &ce); r != ENG_OK) return r;
        if (EngResult r = checkSize(*ce, size); r != ENG_OK) return r;
        if (!bytes) return fail(ENG_ERROR_INVALID_ARGUMENT, "bytes is NULL");
        ecs_set_id(raw(w), e, ce->id, size, bytes);   // OnSet: the change stream sees it
        return ENG_OK;
    });
}

// ── Queries ──────────────────────────────────────────────────────────────────
EngResult engCreateQuery(EngWorld* w, const EngQueryDesc* in, EngQuery* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = 0;
        if (!in || in->struct_size < sizeof(uint32_t)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc is NULL or has no struct_size (start it with ENG_INIT)");
        EngQueryDesc d{};
        std::memcpy(&d, in, std::min<size_t>(in->struct_size, sizeof d));
        if (!d.term_count || d.term_count > kMaxTerms)
            return fail(ENG_ERROR_INVALID_ARGUMENT, "term_count must be 1 to " + std::to_string(kMaxTerms) +
                                                    "; it was " + std::to_string(d.term_count));
        if (!d.terms) return fail(ENG_ERROR_INVALID_ARGUMENT, "terms is NULL");
        QueryEntry qe;
        ecs_query_desc_t qd{};
        bool anyColumnOrWith = false;
        for (uint32_t i = 0; i < d.term_count; ++i) {
            const EngQueryTerm& t = d.terms[i];
            const CompEntry* ce;
            if (EngResult r = checkComp(w, t.component, &ce); r != ENG_OK) return r;
            if (t.reserved[0] || t.reserved[1]) return fail(ENG_ERROR_INVALID_ARGUMENT, "term " + std::to_string(i) + ": reserved must be zero");
            if (t.access > ENG_ACCESS_WRITE_MARKED) return fail(ENG_ERROR_INVALID_ARGUMENT, "term " + std::to_string(i) + ": unknown access " + std::to_string(t.access));
            ecs_term_t& term = qd.terms[i];
            term.id = ce->id;
            term.src.id = EcsSelf;   // rows of this entity: never inherited, so every column is an array
            switch (t.access) {
                case ENG_ACCESS_READ:    term.inout = EcsIn; break;
                case ENG_ACCESS_WRITE:
                case ENG_ACCESS_WRITE_MARKED: term.inout = EcsInOut; break;
                case ENG_ACCESS_WITH:    term.inout = EcsInOutNone; break;
                case ENG_ACCESS_WITHOUT: term.inout = EcsInOutNone; term.oper = EcsNot; break;
            }
            if (t.access != ENG_ACCESS_WITHOUT) anyColumnOrWith = true;
            qe.access.push_back(t.access);
            qe.ids.push_back(ce->id);
            qe.sizes.push_back(ce->size);
            const bool writes = t.access == ENG_ACCESS_WRITE || t.access == ENG_ACCESS_WRITE_MARKED;
            qe.blobTerms.push_back(writes && !ce->blobOffsets.empty() ? ce : nullptr);
        }
        if (!anyColumnOrWith) return fail(ENG_ERROR_INVALID_ARGUMENT, "a query of WITHOUT terms only would match every entity");
        qd.cache_kind = EcsQueryCacheAuto;
        qe.q = ecs_query_init(raw(w), &qd);
        if (!qe.q) return fail(ENG_ERROR_INTERNAL, "the query could not be built");
        // The C kit whose code runs now owns it, so it goes at the kit's stop
        // (engInternalDropOwned), in a reused slot as in a new one. It was set
        // only on the new-slot path, so a kit's second Play left its queries
        // behind (review, 2026-11-28; eng_world_scene_test).
        qe.owner = w->owner;
        // Reuse a free slot, with its generation bumped, so a stale handle is refused.
        for (size_t i = 0; i < w->queries.size(); ++i) {
            if (w->queries[i].q) continue;
            qe.gen = (uint16_t)(w->queries[i].gen + 1);
            if (!qe.gen) qe.gen = 1;
            w->queries[i] = std::move(qe);
            *out = ((EngQuery)w->queries[i].gen << 16) | (EngQuery)(i + 1);
            return ENG_OK;
        }
        if (w->queries.size() >= 0xFFFF) { ecs_query_fini(qe.q); return fail(ENG_ERROR_OUT_OF_MEMORY, "65 535 queries are held"); }
        qe.gen = 1;
        w->queries.push_back(std::move(qe));
        *out = ((EngQuery)1 << 16) | (EngQuery)w->queries.size();
        return ENG_OK;
    });
}

void engDestroyQuery(EngWorld* w, EngQuery q) {
    guard<int>(0, [&] {
        if (checkWorld(w) != ENG_OK) return 0;
        QueryEntry* qe = queryOf(w, q);
        if (!qe) { engInternalSetError("engDestroyQuery: not a live query of this world"); return 0; }
        if (qe->open) { engInternalSetError("engDestroyQuery refused: the query has an open iteration"); return 0; }
        for (uint32_t u : qe->users)
            if (w->systems[u].live) {
                engInternalSetError("engDestroyQuery refused: system '" + w->systems[u].name + "' runs it (engDestroySystem it first)");
                return 0;
            }
        ecs_query_fini(qe->q);
        qe->q = nullptr;
        return 0;
    });
}

EngResult engQueryBegin(EngWorld* w, EngQuery q, EngQueryIter** out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = nullptr;
        QueryEntry* qe = queryOf(w, q);
        if (!qe) return fail(ENG_ERROR_STALE_HANDLE, "not a live query of this world");
        auto it = std::make_unique<EngQueryIter>();
        it->world = w;
        it->query = q & 0xFFFFu;
        it->cols.assign(qe->ids.size(), nullptr);
        ecs_defer_begin(raw(w));   // structural changes wait for End
        it->it = ecs_query_iter(raw(w), qe->q);
        it->started = true;
        ++qe->open;
        ++w->openIters;
        *out = it.release();
        return ENG_OK;
    });
}

EngBool engQueryNext(EngQueryIter* it, EngQueryBatch* out) {
    return guard<EngBool>(ENG_FALSE, [&]() -> EngBool {
        if (!it || !out) { engInternalSetError("iter or out is NULL"); return ENG_FALSE; }
        if (out->struct_size < sizeof(uint32_t)) { engInternalSetError("the batch's struct_size is not set (start it with ENG_INIT)"); return ENG_FALSE; }
        if (it->done) return ENG_FALSE;
        EngWorld* w = it->world;
        QueryEntry& qe = w->queries[it->query - 1];
        settleBatch(it);
        for (;;) {
            if (!ecs_query_next(&it->it)) { it->done = true; return ENG_FALSE; }
            if (it->it.count) break;
        }
        const uint32_t n = (uint32_t)it->it.count;
        for (size_t i = 0; i < qe.ids.size(); ++i) {
            const EngAccess a = qe.access[i];
            if (a == ENG_ACCESS_WITH || a == ENG_ACCESS_WITHOUT) { it->cols[i] = nullptr; continue; }
            it->cols[i] = ecs_field_w_size(&it->it, qe.sizes[i], (int8_t)i);
            // ENG_ACCESS_WRITE: the whole column counts as changed.
            if (a == ENG_ACCESS_WRITE) markRows(w, qe.ids[i], it->it.entities, n);
        }
        EngQueryBatch b{};
        b.struct_size = out->struct_size;
        b.count = n;
        b.entities = it->it.entities;
        b.columns = it->cols.data();
        std::memcpy(out, &b, std::min<size_t>(out->struct_size, sizeof b));
        it->batch = true;
        return ENG_TRUE;
    });
}

void engQueryEnd(EngQueryIter* it) {
    guard<int>(0, [&] {
        if (!it) return 0;
        EngWorld* w = it->world;
        settleBatch(it);
        if (!it->done) ecs_iter_fini(&it->it);
        QueryEntry& qe = w->queries[it->query - 1];
        if (qe.open) --qe.open;
        if (w->openIters) --w->openIters;
        delete it;
        ecs_defer_end(raw(w));     // the queued structural changes apply here
        return 0;
    });
}

void engMarkChanged(EngQueryIter* it, uint32_t term, uint32_t first, uint32_t count) {
    guard<int>(0, [&] {
        if (!it || it->done) { engInternalSetError("engMarkChanged: no current batch"); return 0; }
        EngWorld* w = it->world;
        QueryEntry& qe = w->queries[it->query - 1];
        if (term >= qe.ids.size() || qe.access[term] != ENG_ACCESS_WRITE_MARKED) {
            engInternalSetError("engMarkChanged: term " + std::to_string(term) + " is not ENG_ACCESS_WRITE_MARKED");
            return 0;
        }
        const uint32_t n = (uint32_t)it->it.count;
        if (first > n || count > n - first) {
            engInternalSetError("engMarkChanged: rows [" + std::to_string(first) + ", " + std::to_string(first + count) +
                                ") are outside the batch of " + std::to_string(n));
            return 0;
        }
        markRows(w, qe.ids[term], it->it.entities + first, count);
        return 0;
    });
}

// ── Blobs ────────────────────────────────────────────────────────────────────
EngResult engCreateBlob(EngWorld* w, const void* bytes, uint32_t size, EngBlob* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = 0;
        if (size > kMaxBlobBytes)
            return fail(ENG_ERROR_INVALID_ARGUMENT, "a blob is at most 16 MiB in this version; size was " + std::to_string(size));
        *out = blobsOf(*w->w)->create(static_cast<const uint8_t*>(bytes), size);
        return ENG_OK;
    });
}

EngResult engGetBlob(EngWorld* w, EngBlob blob, const void** outBytes, uint32_t* outSize) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!outBytes || !outSize) return fail(ENG_ERROR_INVALID_ARGUMENT, "out_bytes or out_size is NULL");
        *outBytes = nullptr;
        *outSize = 0;
        if (!blob) return fail(ENG_ERROR_INVALID_ARGUMENT, "blob is 0");
        const BlobStore* st = storeOf(w);
        const uint8_t* p = nullptr;
        uint32_t n = 0;
        if (!st || !st->bytes(blob, p, n)) return fail(ENG_ERROR_STALE_HANDLE, "blob " + std::to_string(blob) + " is not live");
        *outBytes = p;
        *outSize = n;
        return ENG_OK;
    });
}

EngResult engSetBlob(EngWorld* w, EngBlob blob, const void* bytes, uint32_t size) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!blob) return fail(ENG_ERROR_INVALID_ARGUMENT, "blob is 0");
        if (size > kMaxBlobBytes)
            return fail(ENG_ERROR_INVALID_ARGUMENT, "a blob is at most 16 MiB in this version; size was " + std::to_string(size));
        BlobStore* st = storeOf(w);
        BlobStore::Slot* sl = st ? st->find(blob) : nullptr;
        if (!sl) return fail(ENG_ERROR_STALE_HANDLE, "blob " + std::to_string(blob) + " is not live");
        if (bytes) sl->bytes.assign(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + size);
        else sl->bytes.assign(size, 0);
        // A change of the component that holds it.
        if (sl->held) { const EngEntity e = sl->entity; markRows(w, sl->component, &e, 1); }
        return ENG_OK;
    });
}

// ── Systems ──────────────────────────────────────────────────────────────────
EngResult engCreateSystem(EngWorld* w, const EngSystemDesc* in, EngSystem* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = 0;
        if (!in || in->struct_size < sizeof(uint32_t)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc is NULL or has no struct_size (start it with ENG_INIT)");
        EngSystemDesc d{};
        std::memcpy(&d, in, std::min<size_t>(in->struct_size, sizeof d));
        if (!d.name || !*d.name) return fail(ENG_ERROR_INVALID_ARGUMENT, "the system has no name");
        const std::string name = d.name;
        if (name.find('.') == std::string::npos || name.front() == '.' || name.back() == '.')
            return fail(ENG_ERROR_INVALID_ARGUMENT, "'" + name + "' is not namespaced (\"kit.name\")");
        if (d.reserved) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": reserved must be zero");
        if (d.phase >= kPhases) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": unknown phase " + std::to_string(d.phase));
        if (!d.fn) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": fn is NULL");
        if (d.parallel) return fail(ENG_ERROR_UNSUPPORTED, name + ": parallel systems are not in this version");
        if (d.run_after_count && !d.run_after) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": run_after_count is not zero but run_after is NULL");
        QueryEntry* qe = nullptr;
        if (d.query && !(qe = queryOf(w, d.query)))
            return fail(ENG_ERROR_STALE_HANDLE, name + ": query " + std::to_string(d.query) + " is not a live query of this world");
        static const char* kPhaseName[kPhases] = {"FIXED_UPDATE", "UPDATE", "LATE_UPDATE"};
        SystemEntry se;
        se.name = name;
        se.phase = d.phase;
        se.query = d.query;
        se.fn = d.fn;
        se.user = d.user;
        for (uint32_t i = 0; i < d.run_after_count; ++i) {
            if (!d.run_after[i] || !*d.run_after[i])
                return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": run_after[" + std::to_string(i) + "] is empty");
            if (name == d.run_after[i]) return fail(ENG_ERROR_INVALID_ARGUMENT, name + ": runs after itself");
            se.after.emplace_back(d.run_after[i]);
        }
        for (const SystemEntry& o : w->systems) {
            if (!o.live) continue;
            if (o.name == name) return fail(ENG_ERROR_REFUSED, "a system named '" + name + "' exists");
            if (o.phase == d.phase) continue;
            for (const std::string& a : se.after)
                if (a == o.name)
                    return fail(ENG_ERROR_REFUSED, name + " (" + kPhaseName[d.phase] + ") runs after " + a +
                                                   ", which is in " + kPhaseName[o.phase] + ": run_after names a system of the same phase");
            for (const std::string& a : o.after)
                if (a == name)
                    return fail(ENG_ERROR_REFUSED, o.name + " (" + kPhaseName[o.phase] + ") runs after " + name +
                                                   ", which would be in " + kPhaseName[d.phase] + ": run_after names a system of the same phase");
        }
        // A free slot, its generation bumped, or a new one.
        uint32_t idx = (uint32_t)w->systems.size();
        for (uint32_t i = 0; i < w->systems.size(); ++i)
            if (!w->systems[i].live && w->systems[i].gen != 0xFFFF) { idx = i; break; }
        if (idx == w->systems.size()) {
            if (idx >= 0xFFFF) return fail(ENG_ERROR_OUT_OF_MEMORY, "65 535 systems are held");
            w->systems.emplace_back();
        }
        se.gen = (uint16_t)(w->systems[idx].gen + 1);
        se.owner = w->owner;
        se.seq = w->nextSeq++;
        SystemEntry before = std::move(w->systems[idx]);
        w->systems[idx] = std::move(se);
        std::string cycle;
        if (sortPhase(w, d.phase, idx, &cycle).empty() && !cycle.empty()) {
            const uint16_t gen = before.gen;
            w->systems[idx] = std::move(before);
            w->systems[idx].gen = gen;
            return fail(ENG_ERROR_REFUSED, name + ": run_after would make a cycle: " + cycle);
        }
        w->systems[idx].live = true;
        w->orderDirty[d.phase] = true;
        if (qe) qe->users.push_back(idx);
        *out = systemHandle(w, idx);
        return ENG_OK;
    });
}

void engDestroySystem(EngWorld* w, EngSystem h) {
    guard<int>(0, [&] {
        if (checkWorld(w) != ENG_OK) return 0;
        uint32_t idx = 0;
        SystemEntry* s = systemOf(w, h, &idx);
        if (!s) { engInternalSetError("engDestroySystem: not a live system of this world"); return 0; }
        s->live = false;
        w->orderDirty[s->phase] = true;
        if (QueryEntry* qe = queryOf(w, s->query))
            qe->users.erase(std::remove(qe->users.begin(), qe->users.end(), idx), qe->users.end());
        return 0;
    });
}

// ── The change stream ────────────────────────────────────────────────────────
EngResult engSubscribeChanges(EngWorld* w, EngComponent c, EngChangeStream* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = 0;
        const CompEntry* ce;
        if (EngResult r = checkComp(w, c, &ce); r != ENG_OK) return r;
        if (w->openIters) return fail(ENG_ERROR_REFUSED, "not between engQueryBegin and engQueryEnd");
        const changes::StreamId id = changes::Streams::ensure(raw(w)).subscribe(ce->id);
        w->streams.push_back(id);
        *out = id;
        return ENG_OK;
    });
}

void engUnsubscribeChanges(EngWorld* w, EngChangeStream s) {
    guard<int>(0, [&] {
        if (checkWorld(w) != ENG_OK) return 0;
        for (size_t i = 0; i < w->streams.size(); ++i) {
            if (w->streams[i] != s) continue;
            w->streams.erase(w->streams.begin() + (std::ptrdiff_t)i);
            if (changes::Streams* st = changes::Streams::of(raw(w))) st->unsubscribe(s);
            return 0;
        }
        engInternalSetError("engUnsubscribeChanges: not a stream of this world");
        return 0;
    });
}

EngResult engReadChanges(EngWorld* w, EngChangeStream s, EngChange* out, uint32_t* count) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWorld(w); r != ENG_OK) return r;
        if (!count) return fail(ENG_ERROR_INVALID_ARGUMENT, "count is NULL");
        const uint32_t cap = *count;
        *count = 0;
        if (cap && !out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        bool mine = false;
        for (changes::StreamId id : w->streams) mine = mine || id == s;
        if (!mine) return fail(ENG_ERROR_STALE_HANDLE, "not a live stream of this world");
        changes::Streams* st = changes::Streams::of(raw(w));
        std::vector<changes::Change> buf(cap);
        const uint32_t n = st ? st->read(s, buf.data(), cap) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            out[i].entity = buf[i].entity;
            out[i].kind = (EngChangeKind)buf[i].kind;
            out[i].reserved = 0;
        }
        *count = n;
        return ENG_OK;
    });
}

} // extern "C"
