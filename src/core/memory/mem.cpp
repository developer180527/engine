// ── mem:: implementation — 2MB block backend + TLSF tagged heaps ────────────
// RECURSION RULE: nothing in this file may allocate through operator new —
// the routed global new (mem_counters.cpp) lands here. Diagnostics use
// fprintf (no std::string), state is constant-initialized statics (no static
// init order hazard), locks are plain std::mutex (no allocation).
#include "core/memory/mem.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "tlsf.h"

namespace mem {
namespace {

constexpr size_t    kBlockShift = 21;                       // 2MB
constexpr size_t    kBlockSize  = size_t(1) << kBlockShift;
constexpr uintptr_t kBlockMask  = ~uintptr_t(kBlockSize - 1);
constexpr size_t    kHeaderSize = 64;                        // one cache line
constexpr size_t    kLargeMin   = kBlockSize / 2;            // >=1MB: direct map
constexpr uint32_t  kMagic      = 0x4D454D42;                // 'BMEM'

struct TagHeap;

// Lives at the base of every 2MB-aligned region we own. For any user pointer
// p, (p & kBlockMask) finds it — that is the entire provenance scheme.
struct BlockHeader {
    uint32_t magic;
    uint8_t  tag;
    uint8_t  isLarge;
    uint16_t _pad;
    size_t   mapBytes;    // whole mapping (large spans may exceed kBlockSize)
    size_t   userBytes;   // large only: the caller's requested size
    TagHeap* heap;        // pool blocks only: owning heap
};
static_assert(sizeof(BlockHeader) <= kHeaderSize, "header must fit the line");

const char* kTagNames[(size_t)Tag::Count] = {
    "Core", "Frame", "Assets", "Rendering", "Animation", "Physics",
    "Scripting", "ECS", "Audio", "Jobs", "Editor",
};

// ── OS mapping (the ONLY syscalls in the manager) ───────────────────────────
std::atomic<uint64_t> g_mappedBytes{0};
std::atomic<uint64_t> g_mapEvents{0};

// 2MB-aligned mapping via over-allocate + trim. size must be page-aligned.
void* mapAligned(size_t size) {
    const size_t over = size + kBlockSize;
    void* raw = ::mmap(nullptr, over, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    const uintptr_t base = ((uintptr_t)raw + kBlockSize - 1) & kBlockMask;
    const size_t head = base - (uintptr_t)raw;
    const size_t tail = over - head - size;
    if (head) ::munmap(raw, head);
    if (tail) ::munmap((void*)(base + size), tail);
    g_mappedBytes.fetch_add(size, std::memory_order_relaxed);
    g_mapEvents.fetch_add(1, std::memory_order_relaxed);
    return (void*)base;
}

// ── Block registry — "is this pointer ours?" ────────────────────────────────
// Open-addressing table of live block bases. Inserts/erases are rare (heap
// growth, large alloc/free) and mutex'd; lookups are lock-free atomic probes
// on the free/realloc hot path. Sized for 64GB of live 2MB regions.
constexpr size_t    kRegSlots = size_t(1) << 15;
constexpr uintptr_t kTombstone = 1;
std::atomic<uintptr_t> g_registry[kRegSlots];   // zero-init: empty
std::mutex             g_regMu;

inline size_t regHash(uintptr_t base) {
    return size_t(((base >> kBlockShift) * 0x9E3779B97F4A7C15ull) >> 49);
}

void regInsert(uintptr_t base) {
    std::lock_guard<std::mutex> lk(g_regMu);
    size_t i = regHash(base);
    for (;;) {
        uintptr_t v = g_registry[i].load(std::memory_order_relaxed);
        if (v == 0 || v == kTombstone) {
            g_registry[i].store(base, std::memory_order_release);
            return;
        }
        i = (i + 1) & (kRegSlots - 1);
    }
}

void regErase(uintptr_t base) {
    std::lock_guard<std::mutex> lk(g_regMu);
    size_t i = regHash(base);
    for (;;) {
        uintptr_t v = g_registry[i].load(std::memory_order_relaxed);
        if (v == base) {
            g_registry[i].store(kTombstone, std::memory_order_release);
            return;
        }
        if (v == 0) return;   // not present
        i = (i + 1) & (kRegSlots - 1);
    }
}

inline bool regHas(uintptr_t base) {
    size_t i = regHash(base);
    for (;;) {
        const uintptr_t v = g_registry[i].load(std::memory_order_acquire);
        if (v == base) return true;
        if (v == 0)    return false;
        i = (i + 1) & (kRegSlots - 1);
    }
}

// ── TagHeap — TLSF growing 2MB at a time ────────────────────────────────────
struct TagHeap {
    std::mutex            mu;
    tlsf_t                tlsf = nullptr;
    std::atomic<uint64_t> cur{0}, peak{0}, allocs{0}, frees{0};
    std::atomic<uint64_t> budget{0};
    std::atomic<bool>     budgetWarned{false};

    bool grow(Tag tag) {
        void* b = mapAligned(kBlockSize);
        if (!b) return false;
        auto* h = (BlockHeader*)b;
        h->magic = kMagic; h->tag = (uint8_t)tag; h->isLarge = 0;
        h->mapBytes = kBlockSize; h->userBytes = 0; h->heap = this;
        void* pool = (char*)b + kHeaderSize;
        if (!tlsf) tlsf = tlsf_create_with_pool(pool, kBlockSize - kHeaderSize);
        else       tlsf_add_pool(tlsf, pool, kBlockSize - kHeaderSize);
        regInsert((uintptr_t)b);
        return true;
    }

    void* allocate(Tag tag, size_t size, size_t align) {
        std::lock_guard<std::mutex> lk(mu);
        if (!tlsf && !grow(tag)) return nullptr;
        void* p = align <= 8 ? tlsf_malloc(tlsf, size)
                             : tlsf_memalign(tlsf, align, size);
        if (!p) {   // fragmented pools: one fresh 2MB block always fits <1MB
            if (!grow(tag)) return nullptr;
            p = align <= 8 ? tlsf_malloc(tlsf, size)
                           : tlsf_memalign(tlsf, align, size);
            if (!p) return nullptr;
        }
        bump(tlsf_block_size(p));
        return p;
    }

    void release(void* p) {
        std::lock_guard<std::mutex> lk(mu);
        const uint64_t sz = tlsf_block_size(p);
        tlsf_free(tlsf, p);
        cur.fetch_sub(sz, std::memory_order_relaxed);
        frees.fetch_add(1, std::memory_order_relaxed);
    }

    size_t liveSize(void* p) {
        std::lock_guard<std::mutex> lk(mu);
        return tlsf_block_size(p);
    }

    void bump(uint64_t sz) {
        const uint64_t c = cur.fetch_add(sz, std::memory_order_relaxed) + sz;
        allocs.fetch_add(1, std::memory_order_relaxed);
        uint64_t pk = peak.load(std::memory_order_relaxed);
        while (c > pk && !peak.compare_exchange_weak(pk, c)) {}
        const uint64_t b = budget.load(std::memory_order_relaxed);
        if (b && c > b && !budgetWarned.exchange(true))
            std::fprintf(stderr, "[mem] BUDGET EXCEEDED: %llu B live "
                         "(budget %llu B) — see mem::logStats\n",
                         (unsigned long long)c, (unsigned long long)b);
    }
};

// Constant-initialized: safe to touch from the first static-init allocation.
TagHeap g_heaps[(size_t)Tag::Count];

inline TagHeap& heap(Tag t) { return g_heaps[(size_t)t]; }

// ── Thread-local tag scope stack (no allocation, fixed depth) ───────────────
constexpr int kScopeDepth = 16;
thread_local Tag t_scopes[kScopeDepth];
thread_local int t_scopeTop = 0;

// ── Large allocations — dedicated aligned mappings ──────────────────────────
void* largeAlloc(Tag tag, size_t size, size_t align) {
    if (align > kBlockSize / 2) return nullptr;   // nobody needs this
    const size_t offset = (kHeaderSize + align - 1) & ~(align - 1);
    const size_t page   = (size_t)::getpagesize();
    const size_t total  = (offset + size + page - 1) & ~(page - 1);
    void* base = mapAligned(total);
    if (!base) return nullptr;
    auto* h = (BlockHeader*)base;
    h->magic = kMagic; h->tag = (uint8_t)tag; h->isLarge = 1;
    h->mapBytes = total; h->userBytes = size; h->heap = nullptr;
    regInsert((uintptr_t)base);
    heap(tag).bump(size);
    return (char*)base + offset;
}

void largeFree(BlockHeader* h) {
    TagHeap& th = heap((Tag)h->tag);
    th.cur.fetch_sub(h->userBytes, std::memory_order_relaxed);
    th.frees.fetch_add(1, std::memory_order_relaxed);
    g_mappedBytes.fetch_sub(h->mapBytes, std::memory_order_relaxed);
    regErase((uintptr_t)h);
    ::munmap((void*)h, h->mapBytes);
}

inline BlockHeader* headerOf(void* p) {
    const uintptr_t base = (uintptr_t)p & kBlockMask;
    return regHas(base) ? (BlockHeader*)base : nullptr;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────
const char* tagName(Tag t) {
    return (size_t)t < (size_t)Tag::Count ? kTagNames[(size_t)t] : "?";
}

Tag currentTag() { return t_scopeTop ? t_scopes[t_scopeTop - 1] : Tag::Core; }
void pushTag(Tag t) { if (t_scopeTop < kScopeDepth) t_scopes[t_scopeTop++] = t; }
void popTag() { if (t_scopeTop > 0) --t_scopeTop; }

void* alloc(size_t size, size_t align, Tag tag) {
    if (size == 0) size = 1;
    if (align < 8) align = 8;
    if (size + align >= kLargeMin) return largeAlloc(tag, size, align);
    return heap(tag).allocate(tag, size, align);
}
void* alloc(size_t size, size_t align) {
    return alloc(size, align, currentTag());
}

void free(void* p) {
    if (!p) return;
    BlockHeader* h = headerOf(p);
    if (!h) { std::free(p); return; }   // foreign: pre-routing / kit-side new
    if (h->isLarge) largeFree(h);
    else            h->heap->release(p);
}

void* realloc(void* p, size_t newSize) {
    if (!p) return alloc(newSize, 8, currentTag());
    if (newSize == 0) { free(p); return nullptr; }
    BlockHeader* h = headerOf(p);
    if (!h) return std::realloc(p, newSize);   // foreign provenance stays std
    const Tag tag = (Tag)h->tag;
    const size_t oldSize = h->isLarge ? h->userBytes : h->heap->liveSize(p);
    if (newSize <= oldSize) return p;          // shrink: keep in place
    void* np = alloc(newSize, 8, tag);
    if (!np) return nullptr;
    std::memcpy(np, p, oldSize);
    free(p);
    return np;
}

bool owns(void* p) { return p && headerOf(p) != nullptr; }

size_t allocSize(void* p) {
    if (!p) return 0;
    BlockHeader* h = headerOf(p);
    if (!h) return 0;
    return h->isLarge ? h->userBytes : h->heap->liveSize(p);
}

TagStats stats(Tag t) {
    TagHeap& h = heap(t);
    return { h.cur.load(std::memory_order_relaxed),
             h.peak.load(std::memory_order_relaxed),
             h.allocs.load(std::memory_order_relaxed),
             h.frees.load(std::memory_order_relaxed),
             h.budget.load(std::memory_order_relaxed) };
}

void setBudget(Tag t, uint64_t bytes) {
    heap(t).budget.store(bytes, std::memory_order_relaxed);
    heap(t).budgetWarned.store(false, std::memory_order_relaxed);
}

uint64_t mappedBytes()   { return g_mappedBytes.load(std::memory_order_relaxed); }
uint64_t mapEventCount() { return g_mapEvents.load(std::memory_order_relaxed); }

void logStats(const char* label) {
    std::fprintf(stderr, "[mem] %s — mapped %.1f MB, %llu map events\n",
                 label, mappedBytes() / (1024.0 * 1024.0),
                 (unsigned long long)mapEventCount());
    for (size_t i = 0; i < (size_t)Tag::Count; ++i) {
        const TagStats s = stats((Tag)i);
        if (!s.allocCount) continue;
        std::fprintf(stderr,
            "      %-10s live %8.2f MB  peak %8.2f MB  alloc %llu  free %llu\n",
            kTagNames[i], s.currentBytes / (1024.0 * 1024.0),
            s.peakBytes / (1024.0 * 1024.0),
            (unsigned long long)s.allocCount, (unsigned long long)s.freeCount);
    }
    std::fflush(stderr);
}

void init() {
    // Warmup is implicit (heaps grow on first use). This exists to log that
    // the manager is live and to be the place default budgets get applied.
    std::fprintf(stderr, "[mem] tagged-heap manager live — 2MB blocks, "
                 "TLSF, %llu map events so far\n",
                 (unsigned long long)mapEventCount());
}

void shutdown() { logStats("shutdown"); }   // report-only by design

} // namespace mem
