#include "core/mem_counters.h"

#include <cstdlib>
#include <new>

#include "core/memory/mem.h"

namespace prof {
MemCounters& memCounters() {
    static MemCounters c;   // atomics only — no allocation in construction
    return c;
}
} // namespace prof

// ── Routing switch ──────────────────────────────────────────────────────────
// ENGINE_MEM_ROUTE=1 (default): global new/delete route through the tagged
// memory manager (mem::alloc with the thread's MEM_SCOPE tag). This is what
// makes std containers land in tagged heaps with zero container rewrites.
// Build with -DENGINE_MEM_ROUTE=0 to fall back to count+malloc — do that for
// AddressSanitizer / leaks / guard-malloc sessions, which need allocations to
// keep system provenance.
//
// mem::free handles FOREIGN pointers (allocated pre-routing or by a dylib's
// system new) by detecting the registry miss and forwarding to std::free, so
// flipping the switch never mismatches an existing pointer.
#ifndef ENGINE_MEM_ROUTE
  #define ENGINE_MEM_ROUTE 1
#endif

#if ENGINE_MEM_COUNT || ENGINE_MEM_ROUTE

// ── The alignment the UNALIGNED operator new must still provide ─────────────
// NOT alignof(std::max_align_t). The standard requires the plain
// `operator new(size_t)` to return storage aligned to
// __STDCPP_DEFAULT_NEW_ALIGNMENT__, and the two are NOT the same number:
//
//   platform            max_align_t   __STDCPP_DEFAULT_NEW_ALIGNMENT__
//   macOS arm64                   8                                 16
//   MSVC x64                      8                                 16
//   Linux x86-64                 16                                 16
//
// libc++ and MSVC's STL both route an over-aligned type through the ALIGNED
// operator new only when `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`. A
// 16-byte-aligned type is therefore NOT over-aligned: it goes through the plain
// overload, relying on the guarantee this replacement was quietly breaking.
//
// What that cost: ozz's SoaTransform and Float4x4 are alignas(16), so every
// std::vector of them came back 8-byte aligned wherever max_align_t is 8. ozz
// writes those buffers with _mm_store_ps, which FAULTS on a misaligned address
// — so animator_system_test segfaulted on Windows x64 and passed everywhere
// else: Linux x64 got 16 from max_align_t, and both arm64 legs tolerate
// unaligned NEON stores. Three green platforms hid a real bug on the fourth.
static constexpr std::size_t kDefaultNewAlign = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

static inline void* engine_global_alloc(std::size_t n, std::size_t align) {
#if ENGINE_MEM_ROUTE
    void* p = mem::alloc(n ? n : 1, align);
#else
    (void)align;   // over-aligned types don't take this path when not routing
    void* p = std::malloc(n ? n : 1);
#endif
#if ENGINE_MEM_COUNT
    if (p) {
        auto& c = prof::memCounters();
        c.allocCount.fetch_add(1, std::memory_order_relaxed);
        c.allocBytes.fetch_add(n, std::memory_order_relaxed);
    }
#endif
    return p;
}
static inline void engine_global_free(void* p) {
#if ENGINE_MEM_COUNT
    if (p) prof::memCounters().freeCount.fetch_add(1, std::memory_order_relaxed);
#endif
#if ENGINE_MEM_ROUTE
    mem::free(p);
#else
    std::free(p);
#endif
}

void* operator new(std::size_t n) {
    void* p = engine_global_alloc(n, kDefaultNewAlign);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return engine_global_alloc(n, kDefaultNewAlign);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void operator delete(void* p) noexcept              { engine_global_free(p); }
void operator delete[](void* p) noexcept            { engine_global_free(p); }
void operator delete(void* p, std::size_t) noexcept { engine_global_free(p); }
void operator delete[](void* p, std::size_t) noexcept { engine_global_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { engine_global_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { engine_global_free(p); }

#if ENGINE_MEM_ROUTE
// Over-aligned forms — only when routing (mem:: honors any power-of-two
// alignment; the counting-only build keeps the system defaults, which pair
// correctly with the system aligned delete).
void* operator new(std::size_t n, std::align_val_t al) {
    void* p = engine_global_alloc(n, (std::size_t)al);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t al) {
    return ::operator new(n, al);
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return engine_global_alloc(n, (std::size_t)al);
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return engine_global_alloc(n, (std::size_t)al);
}
void operator delete(void* p, std::align_val_t) noexcept   { engine_global_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { engine_global_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept   { engine_global_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { engine_global_free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept   { engine_global_free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { engine_global_free(p); }
#endif // ENGINE_MEM_ROUTE

#endif // ENGINE_MEM_COUNT || ENGINE_MEM_ROUTE
