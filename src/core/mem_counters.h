#pragma once
#include <atomic>
#include <cstdint>

// ── Process-wide C++ allocation counters ────────────────────────────────────
// SAFE BY DESIGN: the global operator new/delete overrides (mem_counters.cpp)
// COUNT and FORWARD to malloc/free. They never pool, never replace the
// allocator, never change a pointer's provenance — so AddressSanitizer,
// `leaks`, Instruments and guard-malloc all keep working. This MEASURES the
// allocator; it does not become one.
//
// Scope of what's visible here: C++ `new`/`delete` only. C `malloc` from C
// libraries (flecs, Lua, SQLite, zlib) does NOT go through operator new — read
// those via the library's own counters (e.g. flecs ecs_os_api_malloc_count),
// which MemoryChannel does. Aligned new (over-aligned types) is also not
// counted in v1 — a measurement gap, not a correctness issue.

#ifndef ENGINE_MEM_COUNT
  #ifdef NDEBUG
    #define ENGINE_MEM_COUNT 0
  #else
    #define ENGINE_MEM_COUNT 1
  #endif
#endif

namespace prof {

struct MemCounters {
    std::atomic<uint64_t> allocCount{0};
    std::atomic<uint64_t> freeCount{0};
    std::atomic<uint64_t> allocBytes{0};
};

// Function-local static (no allocation in its construction) so it is safe to
// touch from the very first operator new, including during static init.
MemCounters& memCounters();

} // namespace prof
