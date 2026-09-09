// ── sim_hash — implementation ───────────────────────────────────────────────
// See sim_hash.h for what this covers and, more importantly, what it does not.
#include "runtime/sim_hash.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "components/event_component.h"     // EventStale — event lifetime is sim state
#include "core/logger.h"

namespace simhash {
namespace {

// The flecs module root owns every builtin. A path beginning "flecs" is theirs.
// Same convention as reflected_serde.h:50, deliberately — two different rules
// for "is this ours" in one tree is how they drift apart.
bool isFlecsOwned(flecs::entity comp) {
    const flecs::string p = comp.path("::", "");
    const char* s = p.c_str();
    return s && std::strncmp(s, "flecs", 5) == 0;
}

const EcsComponent* componentInfo(flecs::world& w, flecs::entity comp) {
    return static_cast<const EcsComponent*>(
        ecs_get_id(w, comp, ecs_id(EcsComponent)));
}

SimStateRegistry& registryOf(flecs::world& w) { return w.ensure<SimStateRegistry>(); }

const SimStateRegistry::Entry* findEntry(const SimStateRegistry& r,
                                         flecs::entity_t comp) {
    for (const auto& e : r.entries) if (e.comp == comp) return &e;
    return nullptr;
}

}  // namespace

// ── Digest ──────────────────────────────────────────────────────────────────
void Digest::bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = engine_abi::mix(h, b[i]);
}

void Digest::f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    // NaN only. +/-0.0 keep their distinct patterns and +/-inf keep theirs:
    // a sign difference is a real difference in the path that produced it, and
    // collapsing it would hide exactly what this is for.
    if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0)
        bits = 0x7fc00000u;                     // canonical quiet NaN
    u64(bits);
}

void Digest::f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
        (bits & 0x000fffffffffffffull) != 0)
        bits = 0x7ff8000000000000ull;
    u64(bits);
}

void Digest::str(std::string_view s) {
    u64(s.size());                              // length prefix: "ab"+"c" must
    bytes(s.data(), s.size());                  // not collide with "a"+"bc"
}

// ── Declaration ─────────────────────────────────────────────────────────────
namespace {
bool registerEntry(flecs::world& w, flecs::entity comp, Classification cls,
                   HashFn fn, const char* reason) {
    if (!comp.is_valid()) {
        LOG_ERROR("SimHash", "classify called with an invalid component");
        return false;
    }
    if (cls == Classification::SimState && !fn) {
        LOG_ERROR("SimHash", "%s declared SimState with no hasher",
                  comp.path("::", "").c_str());
        return false;
    }
    if (cls == Classification::SimExempt && (!reason || !*reason)) {
        LOG_ERROR("SimHash", "%s exempted with no reason — an exemption with no "
                  "written reason is indistinguishable from an oversight",
                  comp.path("::", "").c_str());
        return false;
    }
    SimStateRegistry& reg = registryOf(w);
    if (findEntry(reg, comp.id())) {
        // Covers BOTH duplicate declaration and declared-then-exempted. One
        // entry per component, mutually exclusive, no last-writer-wins.
        LOG_ERROR("SimHash", "%s is already classified — a component has exactly "
                  "one classification", comp.path("::", "").c_str());
        return false;
    }
    // The marker tag on the component TYPE, so the classification is visible
    // from the world itself and not only from this vector.
    if (cls == Classification::SimState) comp.add<SimState>();
    else                                 comp.add<SimExempt>();

    reg.entries.push_back({ comp.id(),
                            static_cast<uint32_t>(reg.entries.size()),
                            comp.path("::", "").c_str(),
                            cls, fn, reason });
    return true;
}
}  // namespace

bool declareId(flecs::world& w, flecs::entity comp, HashFn fn) {
    return registerEntry(w, comp, Classification::SimState, fn, nullptr);
}
bool exemptId(flecs::world& w, flecs::entity comp, const char* reason) {
    return registerEntry(w, comp, Classification::SimExempt, nullptr, reason);
}

// ── Auditability ────────────────────────────────────────────────────────────
bool isAuditableComponent(flecs::world& w, flecs::id id) {
    if (id.is_pair()) return false;              // pairs handled separately
    flecs::entity comp = id.entity();
    if (!comp.is_valid() || isFlecsOwned(comp)) return false;
    const EcsComponent* ci = componentInfo(w, comp);
    return ci && ci->size > 0;                   // zero-size tags carry no state
}

// ── hashWorld ───────────────────────────────────────────────────────────────
uint64_t hashWorld(flecs::world& w, HashReport* detail) {
    const SimStateRegistry& reg = registryOf(w);

    // Entities carrying at least one SimState component. Collected via the
    // registry rather than a world-wide scan: this is the hot path, and the
    // world-wide scan belongs to auditCoverage, which runs twice a run.
    std::vector<flecs::entity_t> ents;
    for (const auto& e : reg.entries) {
        if (e.cls != Classification::SimState) continue;
        ecs_iter_t it = ecs_each_id(w, e.comp);
        while (ecs_each_next(&it))
            for (int32_t i = 0; i < it.count; ++i) ents.push_back(it.entities[i]);
    }
    std::sort(ents.begin(), ents.end());
    ents.erase(std::unique(ents.begin(), ents.end()), ents.end());

    const flecs::entity_t staleRel = w.component<EventStale>().id();

    Digest world;
    world.u64(ents.size());          // structural: entity COUNT is state too
    for (flecs::entity_t raw : ents) {
        flecs::entity e(w, raw);
        Digest ed;
        ed.u64(raw);                 // id + generation: creation order is observable

        for (const auto& entry : reg.entries) {
            if (entry.cls != Classification::SimState) continue;
            const void* p = ecs_get_id(w, raw, entry.comp);
            if (!p) continue;
            ed.u32(entry.canonicalIndex);       // WHICH component, not just its value
            entry.hash(e, p, ed);
        }

        // ── The ChildOf target ─────────────────────────────────────────────
        // A parent link is simulation state: it decides the entity's WORLD
        // pose, which is what physics spawns against and what the renderer
        // draws, while `Transform` — the only thing hashed until now — stays
        // identical through a reparent. So the whole class of "the same local
        // pose under a different ancestor" was invisible to this gate.
        //
        // 0 when unparented, so REMOVING a parent is a change rather than
        // silence. The raw id is already this function's sort key and all
        // twelve pairs are green on it, so hashing one more adds no
        // instability the hash does not already depend on.
        ed.u64((uint64_t)ecs_get_target(w, raw, EcsChildOf, 0));

        // (EventStale, T) — an event's age decides whether a consumer still
        // sees it, so it is simulation state.
        std::vector<uint32_t> stale;
        for (const auto& entry : reg.entries) {
            if (ecs_has_id(w, raw, ecs_pair(staleRel, entry.comp)))
                stale.push_back(entry.canonicalIndex);
        }
        std::sort(stale.begin(), stale.end());
        ed.u64(stale.size());
        for (uint32_t i : stale) ed.u32(i);

        world.u64(ed.h);
        if (detail) detail->perEntity.emplace_back(raw, ed.h);
    }

    if (detail) {
        detail->total = world.h;
        // Per-type digests are a SEPARATE pass on purpose: the same component
        // hashed across every entity, so a divergence report can name the type
        // before it names the entity.
        for (const auto& entry : reg.entries) {
            if (entry.cls != Classification::SimState) continue;
            Digest td;
            for (flecs::entity_t raw : ents) {
                const void* p = ecs_get_id(w, raw, entry.comp);
                if (!p) continue;
                td.u64(raw);
                entry.hash(flecs::entity(w, raw), p, td);
            }
            detail->perType.emplace_back(entry.debugName, td.h);
        }
        // (EventStale, T) presence, as its own reported "type". Without this a
        // divergence in EVENT AGE — which is simulation state, and exactly what
        // a thread-race in collision dispatch produces — showed up as a
        // differing entity digest with NO component type named, leaving the
        // report pointing at an entity and unable to say why. Found by the gate
        // reporting precisely that.
        Digest sd;
        for (flecs::entity_t raw : ents) {
            for (const auto& entry : reg.entries) {
                if (ecs_has_id(w, raw, ecs_pair(staleRel, entry.comp))) {
                    sd.u64(raw);
                    sd.u32(entry.canonicalIndex);
                }
            }
        }
        detail->perType.emplace_back("(EventStale pairs)", sd.h);
    }
    return world.h;
}

// ── auditCoverage ───────────────────────────────────────────────────────────
bool auditCoverage(flecs::world& w, std::vector<std::string>& unclassified) {
    const SimStateRegistry& reg = registryOf(w);
    unclassified.clear();

    ecs_query_desc_t qd = {};
    qd.terms[0].id = EcsAny;
    ecs_query_t* q = ecs_query_init(w, &qd);
    if (!q) {
        LOG_ERROR("SimHash", "auditCoverage could not build its query");
        return false;
    }

    ecs_iter_t it = ecs_query_iter(w, q);
    while (ecs_query_next(&it)) {
        for (int32_t i = 0; i < it.count; ++i) {
            flecs::entity e(w, it.entities[i]);
            // A component TYPE entity is a declaration, not simulation data —
            // and it carries the SimState/SimExempt tags themselves, which
            // would otherwise audit as unclassified components.
            if (e.has<flecs::Component>()) continue;

            e.each([&](flecs::id id) {
                if (!isAuditableComponent(w, id)) return;
                if (findEntry(reg, id.entity().id())) return;
                std::string path = id.entity().path("::", "").c_str();
                if (std::find(unclassified.begin(), unclassified.end(), path)
                    == unclassified.end())
                    unclassified.push_back(std::move(path));
            });
        }
    }
    ecs_query_fini(q);

    std::sort(unclassified.begin(), unclassified.end());
    return unclassified.empty();
}

}  // namespace simhash
