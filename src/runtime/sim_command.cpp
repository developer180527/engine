// ── sim_command — implementation ────────────────────────────────────────────
#include "runtime/sim_command.h"

#include <algorithm>
#include <cstring>

// FNV from addon_protocol.h, NOT game_module.h. game_module.h's engine_abi::mix
// is the same algorithm, but that header pulls <flecs.h> — and this TU lives in
// engine_core precisely so a server, a replay tool or a netcode client can use
// the command record without the ECS. addon_protocol.h is header-only and
// stdlib-only by design (it is meant to be vendored as one file), which is
// exactly the property needed here.
#include <engine/addon_protocol.h>
#include <string_view>
#include "core/logger.h"

namespace simcmd {

bool Buffer::submit(const SimCommand& c) {
    if (c.entity == 0) {
        // An unassigned EntityId cannot be replayed: on the next run it would
        // name a different entity, or none. Refusing here is what keeps a
        // recorded stream meaningful, and it is a caller bug either way —
        // entity_id_util.h assigns ids to everything the scene owns.
        LOG_WARN("SimCmd", "command with entity id 0 refused (kind %u) — an "
                 "unassigned EntityId is not replayable",
                 (unsigned)c.kind);
        return false;
    }
    if (!executable(c.kind)) {
        // RESERVED (or garbage past Count). Refused rather than recorded: a
        // recorded stream must contain only commands the engine executed, or a
        // replay silently does nothing where the original did something — and
        // a reserved value in a shipped take would be unremovable. Warned once
        // per kind, since a caller doing this does it every tick.
        const uint32_t k   = (uint32_t)c.kind;
        const uint32_t bit = k < 31u ? (1u << k) : (1u << 31);
        if (!(m_warnedReserved & bit)) {
            m_warnedReserved |= bit;
            LOG_WARN("SimCmd", "command kind %u is RESERVED and has no executor "
                     "— refused rather than recorded (see simcmd::Cmd)", k);
        }
        return false;
    }
    if (!m_open) {
        // Outside broadcastUpdate. Accepting this would put the command in
        // whichever tick's buffer is open when the caller happened to run,
        // making the tick's command set frame-rate-dependent — see the header.
        // Warned once per tick, counted always: a render-rate caller would
        // otherwise produce one line per frame and drown the log it needs.
        if (m_refused == 0)
            LOG_WARN("SimCmd", "command (kind %u) submitted outside the update "
                     "phase and refused — submit from onUpdate, not onFrame",
                     (unsigned)c.kind);
        ++m_refused;
        return false;
    }
    SimCommand copy = c;
    copy.seq = m_seq++;
    m_cmds.push_back(copy);
    return true;
}

void Buffer::sortForExecution() {
    // STABLE ORDER FROM UNSTABLE SUBMISSION. Two systems submitting for the
    // same entity in either order execute identically; `seq` is only the final
    // tie-break, so it never lets submission order decide an outcome that
    // (entity, source, kind) has already settled.
    std::sort(m_cmds.begin(), m_cmds.end(),
        [](const SimCommand& x, const SimCommand& y) {
            if (x.entity != y.entity) return x.entity < y.entity;
            if (x.source != y.source) return x.source < y.source;
            if (x.kind   != y.kind)   return x.kind   < y.kind;
            return x.seq < y.seq;
        });
}

uint64_t Buffer::digest() const {
    // Hashes the RAW BYTES, which is only sound because SimCommand is
    // padding-free (the static_asserts in the header) — the same reason
    // InputSnapshot can be memcmp'd.
    // Length-prefixed, so a buffer of N commands cannot collide with a
    // different split of the same bytes — the same reason simhash::Digest::str
    // prefixes strings.
    const uint64_t n = m_cmds.size();
    uint64_t h = engine::addon::fnv1a64(
        std::string_view(reinterpret_cast<const char*>(&n), sizeof n));
    for (const SimCommand& c : m_cmds) {
        const uint64_t part = engine::addon::fnv1a64(
            std::string_view(reinterpret_cast<const char*>(&c), sizeof c));
        h ^= part; h *= 1099511628211ull;
    }
    return h;
}

}  // namespace simcmd
