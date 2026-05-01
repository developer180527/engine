#pragma once

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
    std::string value;
};
