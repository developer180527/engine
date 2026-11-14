#pragma once
#include "core/memory/mem.h"
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
// BACKING: the block and any overflow come from the engine's heap (mem::alloc)
// under the tag given to init(), so arenas show up in the memory telemetry.
// They used to come from malloc and were invisible to it.
//
// mark()/rewind() make it a STACK: a scope takes a mark, allocates, and rewinds,
// freeing only what it allocated. jobs::ScratchScope uses this so a job that
// runs inside another job's wait does not free the outer job's scratch.
namespace mem {

class FrameArena {
public:
    FrameArena() = default;
    ~FrameArena() { shutdown(); }
    FrameArena(const FrameArena&)            = delete;
    FrameArena& operator=(const FrameArena&) = delete;

    // Reserve the backing block once, from the engine's heap.
    void init(size_t bytes, Tag tag = Tag::Frame) {
        shutdown();
        m_tag      = tag;
        m_base     = (uint8_t*)mem::alloc(bytes, 64, tag);
        m_capacity = m_base ? bytes : 0;
        m_offset   = 0;
        m_highWater = 0;
    }
    void shutdown() {
        freeOverflow(0);
        mem::free(m_base);
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
        // bytes <= capacity first: a huge `bytes` wraps newOff to a small number.
        if (m_base && bytes <= m_capacity && newOff <= m_capacity) {
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
        freeOverflow(0);
    }

    // A position to come back to. rewind(m) frees everything allocated since
    // mark() returned m, and nothing allocated before it.
    struct Mark { size_t offset; size_t overflowCount; };
    Mark mark() const { return { m_offset, m_overflow.size() }; }
    void rewind(Mark m) {
        if (m.offset <= m_offset) m_offset = m.offset;
        freeOverflow(m.overflowCount);
    }
    bool initialised() const { return m_base != nullptr; }

    size_t used()          const { return m_offset; }
    size_t capacity()      const { return m_capacity; }
    size_t highWater()     const { return m_highWater; } // peak used across frames
    size_t overflowBytes() const { return m_overflowBytes; }

private:
    void* overflowAlloc(size_t bytes, size_t align) {
        warnOverflowOnce();
        void* p = mem::alloc(bytes, align, m_tag);
        if (p) { m_overflow.push_back({p, bytes}); m_overflowBytes += bytes; }
        return p;
    }
    // Frees overflow blocks from index `keep` on (0: all of them).
    void freeOverflow(size_t keep) {
        while (m_overflow.size() > keep) {
            m_overflowBytes -= m_overflow.back().bytes;
            mem::free(m_overflow.back().p);
            m_overflow.pop_back();
        }
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
    struct Overflow { void* p; size_t bytes; };
    std::vector<Overflow> m_overflow;       // tracked fallback blocks
    Tag                m_tag = Tag::Frame;
    size_t             m_overflowBytes = 0;
};

} // namespace mem
