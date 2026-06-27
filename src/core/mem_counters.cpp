#include "core/mem_counters.h"

#include <cstdlib>
#include <new>

namespace prof {
MemCounters& memCounters() {
    static MemCounters c;   // atomics only — no allocation in construction
    return c;
}
} // namespace prof

#if ENGINE_MEM_COUNT
// ── Global operator new/delete: COUNT + FORWARD ─────────────────────────────
// The matched standard set (plain + array + nothrow + sized-delete), all
// routed to malloc/free so new<->delete provenance stays consistent. Aligned
// new/delete (over-aligned types) is intentionally left to the default
// implementation — it pairs with the default aligned delete, so there is no
// mismatch; those allocations are simply not counted.

static inline void* engine_counted_alloc(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (p) {
        auto& c = prof::memCounters();
        c.allocCount.fetch_add(1, std::memory_order_relaxed);
        c.allocBytes.fetch_add(n, std::memory_order_relaxed);
    }
    return p;
}
static inline void engine_counted_free(void* p) {
    if (p) prof::memCounters().freeCount.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

void* operator new(std::size_t n) {
    void* p = engine_counted_alloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return engine_counted_alloc(n);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void operator delete(void* p) noexcept              { engine_counted_free(p); }
void operator delete[](void* p) noexcept            { engine_counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { engine_counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { engine_counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { engine_counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { engine_counted_free(p); }
#endif // ENGINE_MEM_COUNT
