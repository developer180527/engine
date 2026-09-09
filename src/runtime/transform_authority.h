#pragma once
// ── transform_authority — who wrote this pose, and were they allowed to ──────
//
// Stage 3b of the command architecture, and deliberately a BACKSTOP rather than
// the mechanism. Stages 1–3a removed the *reasons* gameplay had to write a
// physics-owned field: locomotion composes through commands, teleport exists,
// kinematic bodies are gameplay-owned. What is left is catching the writes that
// happen anyway — in a kit, in a script, in a system nobody remembered — and
// turning a silent drop into a named report.
//
// ── THE AUTHORITY RULE, PER FIELD ───────────────────────────────────────────
// Per field, not per entity, and the distinction is what makes this usable. An
// earlier draft hashed ONE digest per entity, which would have reported a
// perfectly legal `scale` write as a violation. A watcher that fires on correct
// code is worse than no watcher: it gets muted, and then it is worse than
// nothing because it looks like coverage.
//
//   entity kind                    position   rotation   scale
//   ---------------------------------------------------------------
//   plain (no physics)             gameplay   gameplay   gameplay
//   RigidBody Static               PHYSICS    PHYSICS    gameplay
//   RigidBody Dynamic              PHYSICS    PHYSICS    gameplay
//   RigidBody Kinematic            gameplay   gameplay   gameplay
//   CharacterController            PHYSICS    gameplay   gameplay
//
// **Kinematic is gameplay-owned, and the plan said otherwise.** It listed
// Kinematic with Dynamic as physics-owned; implementing BUG-0058 showed that is
// backwards. A kinematic body's pose is by definition the one gameplay decides
// — that is what distinguishes it from a dynamic one — so the backend follows
// the Transform and never writes it back. Nothing to watch.
//
// **A character's rotation is gameplay-owned even though physics reads it.**
// `pushEcsToPhysics` pushes it INTO `CharacterVirtual` and never reads it back
// (BUG-0059). Reading is not owning.
//
// **Scale is never backend-owned.** `spawnBody` sizes shapes from `RigidBody`'s
// half-extents, not from `Transform.scale`, so scaling a body at runtime does
// not resize its collider. That is a real limitation, recorded in
// open-questions.md — but it is not an authority violation, and this must not
// report it as one.
//
// ── WHAT IT CANNOT SEE, STATED PRECISELY ────────────────────────────────────
// A write UNDONE WITHIN ONE PHASE. A previous draft claimed such a write
// "cannot alter the simulation's result"; that is too strong and a reviewer was
// right to reject it — another system reading `Transform` mid-phase can branch
// on the transient value or emit a side effect from it.
//
// The correct statement is narrower and is three instruments, not one:
//   * this watcher detects FINAL-STATE ownership violations,
//   * the determinism gate detects OBSERVABLE DIVERGENCE,
//   * neither detects a transient read-back.
// Under stages 1–3a the class shrinks anyway, because gameplay has no reason to
// write these fields at all.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <flecs.h>

#include "components/character_controller.h"
#include "components/rigid_body.h"
#include "core/logger.h"
#include "core/transform.h"
#include "runtime/world_query_cache.h"

// Compile switch, three levels, matching ENGINE_PROFILE's shape:
//   2  full — per-entity, per-field, per-phase records (debug default)
//   1  count only — the same comparison without the records or the strings
//   0  compiled out entirely; every call below becomes nothing
#ifndef ENGINE_TRANSFORM_AUTHORITY
  #ifdef NDEBUG
    #define ENGINE_TRANSFORM_AUTHORITY 1
  #else
    #define ENGINE_TRANSFORM_AUTHORITY 2
  #endif
#endif

namespace authority {

// Which Transform fields the physics backend owns for one entity.
enum Field : uint8_t {
    None     = 0,
    Position = 1 << 0,
    Rotation = 1 << 1,
    // Scale is deliberately absent — see the header comment. Leaving it out of
    // the enum rather than out of the table is what stops a later edit from
    // quietly adding it back.
};

// The rule, in one place. Everything else in this file is bookkeeping.
inline uint8_t backendOwned(flecs::entity e) {
    if (e.has<CharacterController>())
        return Position;                  // rotation is pushed in, not read back
    if (const RigidBody* rb = e.try_get<RigidBody>()) {
        switch (rb->bodyType) {
        case PhysicsBodyType::Kinematic: return None;   // gameplay drives it
        case PhysicsBodyType::Static:
        case PhysicsBodyType::Dynamic:   return Position | Rotation;
        default:                         return Position | Rotation;
        }
    }
    return None;
}

inline const char* fieldName(uint8_t f) {
    if ((f & (Position | Rotation)) == (Position | Rotation)) return "position+rotation";
    if (f & Position) return "position";
    if (f & Rotation) return "rotation";
    return "none";
}

struct Violation {
    uint64_t    entity = 0;      // flecs::entity_t — this is a live-process tool
    std::string name;            // the entity's Name, if it has one
    uint8_t     fields = 0;
    std::string phase;
};

class Watcher {
public:
#if ENGINE_TRANSFORM_AUTHORITY == 0
    void rebase(flecs::world&) {}
    void check(flecs::world&, const char*) {}
    void reset() {}
    size_t violations() const { return 0; }
    const std::vector<Violation>& recent() const { static std::vector<Violation> k; return k; }
    static constexpr bool active = false;
#else
    static constexpr bool active = true;

    // Take the current pose of every watched entity as the new baseline.
    // Called after each LEGITIMATE writer, so the next check compares against
    // what that writer left rather than against the start of the tick.
    void rebase(flecs::world& w) { sweep(w, nullptr); }

    // Compare against the baseline and report anything that moved in a field
    // its owner did not write. Re-baselines as it goes, so one bad write is
    // reported ONCE rather than by every phase for the rest of the tick.
    void check(flecs::world& w, const char* phase) { sweep(w, phase); }

    // Call when the world this watched is destroyed — the cached query would
    // otherwise false-match a new world allocated at the same address, which is
    // the lifetime rule WorldQueryCache states.
    void reset() {
        m_base.clear(); m_recent.clear(); m_warned.clear();
        m_violations = 0; m_query.reset();
    }
    size_t violations() const { return m_violations; }
    const std::vector<Violation>& recent() const { return m_recent; }

private:
    struct Snap { bx::Vec3 pos; bx::Quaternion rot; };

    // EXACT comparison, not a tolerance. The question is "did anyone write
    // this", and a write of a value one ULP away is still a write — by a system
    // that believes it owns the field. A tolerance here would hide precisely
    // the small, repeated, accumulating writes that are hardest to find by
    // hand.
    static bool samePos(const bx::Vec3& a, const bx::Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    static bool sameRot(const bx::Quaternion& a, const bx::Quaternion& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    // One pass. `phase == nullptr` means rebase-only (no reporting).
    void sweep(flecs::world& w, const char* phase) {
        m_seen.clear();
        // Cached: a query is built per WORLD, not per sweep. This runs five
        // times a tick, and building a query allocates and re-matches
        // archetypes — the cost this cache exists for.
        m_query.get(w).each([&](flecs::entity e, const Transform& t) {
                const uint8_t owned = backendOwned(e);
                if (owned == None) return;
                m_seen.push_back(e.id());

                auto it = m_base.find(e.id());
                if (it == m_base.end()) {
                    // First sight — nothing to compare against. An entity that
                    // appears mid-tick is not a violation; it is a spawn.
                    m_base.emplace(e.id(), Snap{ t.position, t.rotation });
                    return;
                }
                if (phase) {
                    uint8_t bad = 0;
                    if ((owned & Position) && !samePos(it->second.pos, t.position))
                        bad |= Position;
                    if ((owned & Rotation) && !sameRot(it->second.rot, t.rotation))
                        bad |= Rotation;
                    if (bad) report(e, bad, phase);
                }
                it->second.pos = t.position;
                it->second.rot = t.rotation;
        });

        // Drop entities that died or stopped being watched, so the map does not
        // grow for the length of a session.
        if (m_base.size() != m_seen.size()) {
            std::unordered_map<uint64_t, Snap> kept;
            kept.reserve(m_seen.size());
            for (uint64_t id : m_seen) {
                auto it = m_base.find(id);
                if (it != m_base.end()) kept.emplace(id, it->second);
            }
            m_base.swap(kept);
        }
    }

    void report(flecs::entity e, uint8_t bad, const char* phase) {
        ++m_violations;
#if ENGINE_TRANSFORM_AUTHORITY >= 2
        Violation v;
        v.entity = e.id();
        v.name   = e.name() ? e.name().c_str() : "";
        v.fields = bad;
        v.phase  = phase;
        // Bounded: a system writing every entity every tick would otherwise
        // turn a diagnostic into a memory leak with a respectable name.
        if (m_recent.size() < kMaxRecords) m_recent.push_back(v);
        // Warn once PER (entity, field, phase). The same bad write repeats every
        // tick, and a per-tick log line buries the report it is trying to make.
        const uint64_t key = e.id() ^ ((uint64_t)bad << 56) ^ hashPhase(phase);
        if (m_warned.insert(key).second) {
            LOG_WARN("Authority", "%s wrote %s of entity %llu (%s), which "
                     "physics owns — the write is discarded at the next step. "
                     "Use a Teleport command, or a MoveContribution for a "
                     "character.",
                     phase, fieldName(bad), (unsigned long long)e.id(),
                     v.name.empty() ? "unnamed" : v.name.c_str());
        }
#else
        (void)e; (void)bad; (void)phase;
#endif
    }

    static uint64_t hashPhase(const char* s) {
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
        return h;
    }

    static constexpr size_t kMaxRecords = 256;
    // Hashed, and only ever looked up — never iterated — so its order cannot
    // reach the simulation. The same argument JoltPlugin's m_charState is kept
    // hashed under, and it matters more here: this runs inside the fixed step.
    std::unordered_map<uint64_t, Snap> m_base;
    std::unordered_set<uint64_t>       m_warned;   // one line per (entity, field, phase)
    std::vector<uint64_t>              m_seen;
    std::vector<Violation>             m_recent;
    size_t                             m_violations = 0;
    WorldQueryCache<const Transform>   m_query;
#endif
};

}  // namespace authority
