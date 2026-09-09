// ── sim_intent — implementation ─────────────────────────────────────────────
#include "runtime/sim_intent.h"

#include <algorithm>
#include <string_view>

// FNV from addon_protocol.h and not game_module.h, for the reason sim_command
// takes it from there: game_module.h pulls <flecs.h>, and this TU lives in
// engine_core so a replay tool or a netcode client can read an intent stream
// with no ECS at all.
#include <engine/addon_protocol.h>
#include "core/logger.h"

namespace simintent {

bool Buffer::submit(const Intent& in) {
    if (in.entity == 0) {
        LOG_WARN("Intent", "intent with entity id 0 refused — an unassigned "
                 "EntityId names a different entity on the next run, so the "
                 "stream would not replay");
        return false;
    }
    if (!m_open) {
        // Outside the tick's window. Accepting it would put the intent in
        // whichever tick happened to be open when the caller ran, which is
        // precisely the frame-rate coupling this layer exists to remove.
        if (m_refused == 0)
            LOG_WARN("Intent", "intent submitted outside the update phase and "
                     "refused — sample once per TICK, not once per frame");
        ++m_refused;
        return false;
    }
    Intent copy = in;
    copy.seq = m_seq++;
    m_intents.push_back(copy);
    return true;
}

void Buffer::sortForExecution() {
    std::sort(m_intents.begin(), m_intents.end(),
        [](const Intent& a, const Intent& b) {
            if (a.entity != b.entity) return a.entity < b.entity;
            if (a.source != b.source) return a.source < b.source;
            return a.seq < b.seq;
        });
}

const Intent* Buffer::find(uint64_t entity, uint16_t source) const {
    // Linear: a tick has one intent per controller, so this is a handful of
    // entries. An index would cost more to maintain than it saves, and would
    // be a second structure to keep canonical.
    for (const Intent& i : m_intents)
        if (i.entity == entity && i.source == source) return &i;
    return nullptr;
}

uint64_t Buffer::digest() const {
    const uint64_t n = m_intents.size();
    uint64_t h = engine::addon::fnv1a64(
        std::string_view(reinterpret_cast<const char*>(&n), sizeof n));
    for (const Intent& i : m_intents) {
        const uint64_t part = engine::addon::fnv1a64(
            std::string_view(reinterpret_cast<const char*>(&i), sizeof i));
        h ^= part; h *= 1099511628211ull;
    }
    return h;
}

// ── ActionSet ───────────────────────────────────────────────────────────────
int ActionSet::declare(const std::string& name) {
    const int existing = indexOf(name);
    if (existing >= 0) return existing;
    if ((int)m_names.size() >= kMaxActions) {
        LOG_WARN("Intent", "action '%s' not declared: the intent bitset holds "
                 "%d actions and all are taken", name.c_str(), kMaxActions);
        return -1;
    }
    m_names.push_back(name);
    return (int)m_names.size() - 1;
}

int ActionSet::indexOf(const std::string& name) const {
    for (size_t i = 0; i < m_names.size(); ++i)
        if (m_names[i] == name) return (int)i;
    return -1;
}

uint64_t ActionSet::declarationHash() const {
    // ORDER-SENSITIVE, because the order IS the meaning: the same names
    // declared in a different order assign different bits, so a stream
    // recorded against one and replayed against the other would act on the
    // wrong actions. Length-prefixed per name so ("Fire","Jump") cannot
    // collide with ("Fire" + "Jump").
    uint64_t h = 1469598103934665603ull;
    const uint64_t n = m_names.size();
    h ^= engine::addon::fnv1a64(
        std::string_view(reinterpret_cast<const char*>(&n), sizeof n));
    for (const std::string& s : m_names) {
        h *= 1099511628211ull;
        h ^= engine::addon::fnv1a64(std::string_view(s));
    }
    return h;
}

}  // namespace simintent
