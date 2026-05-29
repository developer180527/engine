#pragma once
#include <cstdint>

// ── EntityId ───────────────────────────────────────────────────────────────
// Stable, persistent identity for a scene entity. Generated once (random
// 64-bit), serialized, and never changed. Parent links, undo records, and any
// cross-reference key on this — NOT on Name, which is mutable/cosmetic and may
// be duplicated. value == 0 means "unassigned".
struct EntityId {
    uint64_t value = 0;
};
