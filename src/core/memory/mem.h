#pragma once
// ── mem:: — the engine memory manager ───────────────────────────────────────
//
// Tagged heaps over a 2MB-block OS backend. The design goal is the same as
// engine::jobs: the OS is a boot/growth-time concern, never a frame concern.
//
//   Layer 0  Block backend — 2MB-ALIGNED regions mmap'd on heap growth and
//            NEVER returned to the OS until process exit (recycled through a
//            free list). Alignment is the whole trick: for any pointer we
//            own, `ptr & ~(2MB-1)` is its block base, where a header names
//            the owning heap — O(1) provenance with zero per-allocation
//            metadata of our own.
//   Layer 1  TagHeap per Tag — a TLSF allocator (third_party/tlsf: O(1)
//            malloc/free, bounded fragmentation, the industry embedded/game
//            standard) growing 2MB at a time. Allocations >= ~1MB bypass
//            TLSF and get dedicated aligned mappings (LARGE blocks).
//   Layer 2  Tags + MEM_SCOPE — every allocation belongs to a Tag. Explicit
//            calls pass it; routed global new (mem_counters.cpp) reads the
//            thread-local scope stack: MEM_SCOPE(Tag::Assets) attributes
//            every std::string/vector/map underneath it, no container
//            rewrites (the Naughty Dog trick).
//   Layer 3  Telemetry — per-tag current/peak/counts + soft budgets, read by
//            MemoryChannel each frame; mapEventCount() is the syscall-
//            minimization metric (steady state target: 0 growth per frame).
//
// LIFECYCLE: lazy-initialized on first use (static-init allocations route
// correctly), intentionally never torn down (like the profiler hub) — statics
// destroyed after main() may still free through us. shutdown() only reports.
//
// FOREIGN POINTERS: free()/realloc() of memory we don't own (allocated before
// routing, or by a kit dylib's system new) detects the registry miss and
// forwards to std::free/std::realloc. The REVERSE direction — our pointer
// reaching a kit's libc++ delete — would crash; kits therefore must not
// free/resize engine-owned containers (already the C-API-first rule; the real
// fix is a shared engine core dylib, tracked in Future Work).

#include <cstddef>
#include <cstdint>

namespace mem {

// Extend freely; keep kTagNames in mem.cpp in sync.
enum class Tag : uint8_t {
    Core = 0,     // untagged engine allocations (default scope)
    Frame,        // per-frame transient (FrameArena backing)
    Assets,       // import/cook/loader CPU-side data
    Rendering,    // bgfx/bx + renderer-side buffers
    Animation,    // ozz runtime + skeleton/clip registries
    Physics,      // Jolt
    Scripting,    // Lua states + script host
    ECS,          // flecs
    Audio,        // miniaudio
    Jobs,         // enkiTS + job control blocks
    Editor,       // ImGui + editor-only state
    Count
};
const char* tagName(Tag t);

// ── Explicit allocation API ─────────────────────────────────────────────────
// tag defaults to the current MEM_SCOPE (Tag::Core at the bottom of stack).
void* alloc(size_t size, size_t align = alignof(max_align_t));
void* alloc(size_t size, size_t align, Tag tag);
void  free(void* p);
void* realloc(void* p, size_t newSize);   // foreign pointers forwarded to std
bool  owns(void* p);                      // registry hit?
size_t allocSize(void* p);                // usable size; 0 if foreign/null

// ── Tag scopes (thread-local stack) ─────────────────────────────────────────
Tag currentTag();
void pushTag(Tag t);
void popTag();

struct ScopedTag {
    explicit ScopedTag(Tag t) { pushTag(t); }
    ~ScopedTag() { popTag(); }
    ScopedTag(const ScopedTag&)            = delete;
    ScopedTag& operator=(const ScopedTag&) = delete;
};
#define MEM_SCOPE_CONCAT2(a, b) a##b
#define MEM_SCOPE_CONCAT(a, b)  MEM_SCOPE_CONCAT2(a, b)
#define MEM_SCOPE(tag) ::mem::ScopedTag MEM_SCOPE_CONCAT(_memScope, __LINE__)(tag)

// ── Telemetry ───────────────────────────────────────────────────────────────
struct TagStats {
    uint64_t currentBytes;   // live user bytes
    uint64_t peakBytes;
    uint64_t allocCount;     // lifetime
    uint64_t freeCount;
    uint64_t budgetBytes;    // 0 = no budget; exceeding logs a warning once
};
TagStats stats(Tag t);
void     setBudget(Tag t, uint64_t bytes);
uint64_t mappedBytes();      // total OS memory held (blocks + large)
uint64_t mapEventCount();    // lifetime mmap calls — THE syscall metric
void     logStats(const char* label);   // one-line-per-active-tag dump

// Optional explicit warmup (EngineRuntime::init calls it to pay first-touch
// costs at boot and to apply default budgets). Safe to skip — everything
// lazy-initializes.
void init();
void shutdown();   // report-only (see LIFECYCLE above)

} // namespace mem
