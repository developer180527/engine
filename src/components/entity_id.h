#pragma once
#include <cstdint>

// ── EntityId ───────────────────────────────────────────────────────────────
// Stable, persistent identity for a scene entity. Generated once (random
// 64-bit), serialized, and never changed. Parent links, undo records, and any
// cross-reference key on this — NOT on Name, which is mutable/cosmetic and may
// be duplicated. value == 0 means "unassigned".
struct EntityId {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    uint64_t value = 0;
};
