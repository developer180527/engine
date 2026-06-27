#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

// ── FrameArena — linear "bump" allocator for per-frame transient data ───────
// alloc() advances a pointer; reset() rewinds it to zero, freeing EVERYTHING
// at once (O(1)) — thousands of temporaries cost one pointer-add each and one
// pointer reset frees them all. No per-object free, no fragmentation.
//
// USE FOR: short-lived per-frame scratch (extraction lists, temp strings,
// command/culling buffers, render-graph nodes). NOT a general allocator.
//
// HARD RULES:
//   • LIFETIME: memory is valid ONLY until the next reset() — i.e. within the
//     frame it was allocated in. NEVER store an arena pointer across frames.
//   • TRIVIAL DESTRUCTION ONLY: reset() does not run destructors. Only put
//     trivially-destructible data here (POD, or types whose cleanup you don't
//     need). Anything owning a resource must be freed explicitly.
//   • NOT THREAD-SAFE: one arena per thread. v1 = one main-thread arena;
//     per-thread arenas arrive with the job system.
//
// Lives in core/ — pure std, zero engine deps (engine_core).
namespace mem {

class FrameArena {
public:
    FrameArena() = default;
    ~FrameArena() { shutdown(); }
    FrameArena(const FrameArena&)            = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    // Reserve the backing block once. (malloc for v1 — a single large block;
    // mmap with lazy commit is a later option for very large arenas.)
    void init(size_t bytes) {
        shutdown();
        m_base     = (uint8_t*)std::malloc(bytes);
        m_capacity = m_base ? bytes : 0;
        m_offset   = 0;
        m_highWater = 0;
    }
    void shutdown() {
        freeOverflow();
        std::free(m_base);
        m_base = nullptr; m_capacity = m_offset = m_highWater = 0;
    }

    // Raw aligned allocation. On overflow, falls back to a tracked heap block
    // (freed at reset) and warns once — graceful, never crashes. A frame that
    // overflows wants a bigger arena.
    void* alloc(size_t bytes, size_t align = alignof(std::max_align_t)) {
        // Align the ABSOLUTE address, not the offset — the base block's own
        // alignment isn't guaranteed past max_align_t.
        const uintptr_t cur     = (uintptr_t)(m_base + m_offset);
        const uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
        const size_t    newOff  = (size_t)(aligned - (uintptr_t)m_base) + bytes;
        if (m_base && newOff <= m_capacity) {
            m_offset = newOff;
            if (m_offset > m_highWater) m_highWater = m_offset;
            return (void*)aligned;
        }
        return overflowAlloc(bytes, align);
    }

    // Typed convenience — does NOT construct (trivial data only).
    template <class T>
    T* alloc(size_t count = 1) {
        return (T*)alloc(sizeof(T) * count, alignof(T));
    }

    // Frees everything allocated since the last reset (call once per frame).
    void reset() {
        m_offset = 0;
        freeOverflow();
    }

    size_t used()          const { return m_offset; }
    size_t capacity()      const { return m_capacity; }
    size_t highWater()     const { return m_highWater; } // peak used across frames
    size_t overflowBytes() const { return m_overflowBytes; }

private:
    void* overflowAlloc(size_t bytes, size_t align) {
        warnOverflowOnce();
        void* p = nullptr;
#if defined(_WIN32)
        p = _aligned_malloc(bytes, align < sizeof(void*) ? sizeof(void*) : align);
#else
        if (align < sizeof(void*)) align = sizeof(void*);
        // size must be a multiple of alignment for aligned_alloc
        size_t sz = (bytes + align - 1) & ~(align - 1);
        if (posix_memalign(&p, align, sz) != 0) p = nullptr;
#endif
        if (p) { m_overflow.push_back(p); m_overflowBytes += bytes; }
        return p;
    }
    void freeOverflow() {
        for (void* p : m_overflow)
#if defined(_WIN32)
            _aligned_free(p);
#else
            std::free(p);
#endif
        m_overflow.clear();
        m_overflowBytes = 0;
    }
    static void warnOverflowOnce() {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "[FrameArena] capacity exceeded — falling back "
                "to heap (raise the arena size). This is graceful but slow.\n");
        }
    }

    uint8_t*           m_base = nullptr;
    size_t             m_capacity = 0, m_offset = 0, m_highWater = 0;
    std::vector<void*> m_overflow;          // tracked fallback blocks
    size_t             m_overflowBytes = 0;
};

} // namespace mem
