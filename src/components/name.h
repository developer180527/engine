#pragma once
#include <cstdint>

#include <string>

// Name component.
//
// Optional human-readable label for an entity. Used by the editor's hierarchy
// panel to display entities, by logging to identify which entity an error
// originated from, and eventually by serialization for stable references
// across save/load.
//
// Not every entity needs a Name. Entities created procedurally (particles,
// projectiles spawned at runtime) typically don't have one. The hierarchy
// panel skips unnamed entities or shows them as "(unnamed)".
struct Name {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    std::string value;
};
