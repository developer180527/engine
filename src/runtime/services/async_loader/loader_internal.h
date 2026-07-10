#pragma once
// ── AsyncLoader internals — shared by the split TUs ─────────────────────────
// loader.cpp (queue/lifecycle), parse.cpp (CPU import), upload.cpp (GPU).
#include <algorithm>
#include <string>

namespace asyncldr {

// Normalize path separators → forward slashes for consistent cache keys.
// On Windows, string() returns backslashes; generic_string() returns /.
// INVARIANT (audit C.4): every store/lookup/erase on m_loadedResults,
// m_inFlight and m_waiters goes through this — no raw path may ever be a
// map key. Half-normalized keying structurally defeated the cache (store
// raw, look up normalized → every load reprocessed) and DROPPED waiter
// callbacks queued under the normalized key that completion looked up raw.
// Raw paths are kept only for actual filesystem access (processFile).
inline std::string normalizeKey(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

} // namespace asyncldr
