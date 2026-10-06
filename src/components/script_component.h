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
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    std::string scriptPath;        // asset reference, serialized
    uint32_t    instanceId = 0;    // runtime only, backend-owned
    bool        started    = false; // runtime only, onStart dispatched?
};
