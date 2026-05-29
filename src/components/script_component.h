#pragma once
#include <string>
#include <cstdint>

// ── ScriptComponent ────────────────────────────────────────────────────────
// Attaches a script to an entity. `scriptPath` references a script asset
// (e.g. "scripts/player.lua"). `instanceId` is a runtime handle owned by the
// active script backend (0 = not yet instantiated). This component is
// deliberately backend-agnostic — the identical component drives a Lua,
// Python, or blueprint backend; only the active backend interprets scriptPath.
struct ScriptComponent {
    std::string scriptPath;        // asset reference, serialized
    uint32_t    instanceId = 0;    // runtime only, backend-owned
    bool        started    = false; // runtime only, onStart dispatched?
};
