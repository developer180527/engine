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
};

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
};

} // namespace

struct EngWorld {
    EngEngine*      engine = nullptr;
    flecs::world*   w = nullptr;
    CapiComponents* table = nullptr;
    std::vector<QueryEntry> queries;
    std::vector<changes::StreamId> streams;
    uint32_t openIters = 0;
};

struct EngQueryIter {
    EngWorld* world = nullptr;
    uint32_t query = 0;              // index
    ecs_iter_t it{};
    bool started = false, done = false;
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
        if (f.type == ENG_FIELD_ASSET || f.type == ENG_FIELD_BLOB)
            return fail(ENG_ERROR_UNSUPPORTED, where + " ('" + f.name + "'): " +
                        (f.type == ENG_FIELD_ASSET ? "ASSET" : "BLOB") + " fields are not in this version");
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

} // namespace

uint64_t engInternalComponentId(EngWorld* w, EngComponent c) {
    const CompEntry* ce = w ? comp(w, c) : nullptr;
    return ce ? ce->id : 0;
}

void engInternalDestroyWorld(EngWorld* w) {
    if (!w) return;
    for (QueryEntry& q : w->queries)
        if (q.q) ecs_query_fini(q.q);
    if (changes::Streams* s = changes::Streams::of(raw(w)))
        for (changes::StreamId id : w->streams) s->unsubscribe(id);
    delete w;
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
        EngWorld*& slot = engInternalWorldSlot(engine);
        if (slot) return fail(ENG_ERROR_UNSUPPORTED, "this version has one world, and its handle is held "
                                                     "(engDestroyWorld it first)");
        EngineRuntime* rt = engInternalRuntime(engine);
        if (!rt) return ENG_ERROR_UNSUPPORTED;   // the message is the start's
        auto w = std::make_unique<EngWorld>();
        w->engine = engine;
        w->w = &rt->simWorld();
        w->table = tableOf(*w->w);
        slot = w.get();
        *out = w.release();
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
        w->table->comps.push_back(std::move(c));
        const CompEntry& kept = w->table->comps.back();
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
        }
        if (!anyColumnOrWith) return fail(ENG_ERROR_INVALID_ARGUMENT, "a query of WITHOUT terms only would match every entity");
        qd.cache_kind = EcsQueryCacheAuto;
        qe.q = ecs_query_init(raw(w), &qd);
        if (!qe.q) return fail(ENG_ERROR_INTERNAL, "the query could not be built");
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
        return ENG_TRUE;
    });
}

void engQueryEnd(EngQueryIter* it) {
    guard<int>(0, [&] {
        if (!it) return 0;
        EngWorld* w = it->world;
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
