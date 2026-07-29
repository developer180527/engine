// ── mem:: implementation — 2MB block backend + TLSF tagged heaps ────────────
// RECURSION RULE: nothing in this file may allocate through operator new —
// the routed global new (mem_counters.cpp) lands here. Diagnostics use
// fprintf (no std::string), state is constant-initialized statics (no static
// init order hazard), locks are immortal OS primitives (no allocation).
#include "core/memory/mem.h"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN trims the socket/RPC/OLE headers; NOMINMAX stops
// windows.h from defining min/max as macros, which breaks <algorithm> and
// any `std::numeric_limits<T>::max()` downstream.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "tlsf.h"

namespace mem {
namespace {

constexpr size_t    kBlockShift = 21;                       // 2MB
constexpr size_t    kBlockSize  = size_t(1) << kBlockShift;
constexpr uintptr_t kBlockMask  = ~uintptr_t(kBlockSize - 1);
constexpr size_t    kHeaderSize = 64;                        // one cache line
constexpr size_t    kLargeMin   = kBlockSize / 2;            // >=1MB: direct map
constexpr uint32_t  kMagic      = 0x4D454D42;                // 'BMEM'

struct Shard;

// Per-tag pools are STRIPED across kShards, each with its own lock. A thread
// picks a shard once (round-robin) and allocates there — so N threads spread
// across N locks instead of serializing on one per-tag mutex (the measured
// stress_swarm contention: 0.32x under 12 threads). A block routes to its
// owning shard on free, so cross-thread frees stay correct. Bigger N = less
// contention but more idle 2MB pools; 16 gives each engine worker its own.
constexpr int kShards = 16;

// Lives at the base of every 2MB-aligned region we own. For any user pointer
// p, (p & kBlockMask) finds it — that is the entire provenance scheme.
struct BlockHeader {
    uint32_t magic;
    uint8_t  tag;
    uint8_t  isLarge;
    uint16_t _pad;
    size_t   mapBytes;    // whole mapping (large spans may exceed kBlockSize)
    size_t   userBytes;   // large only: the caller's requested size
    Shard*   shard;       // pool blocks only: owning shard (for free routing)
    void*    mapRaw;      // what goes back to the OS; == this except on Windows
};
static_assert(sizeof(BlockHeader) <= kHeaderSize, "header must fit the line");

const char* kTagNames[(size_t)Tag::Count] = {
    "Core", "Frame", "Assets", "Rendering", "Animation", "Physics",
    "Scripting", "ECS", "Audio", "Jobs", "Editor",
};

// ── OS mapping (the ONLY syscalls in the manager) ───────────────────────────
std::atomic<uint64_t> g_mappedBytes{0};
std::atomic<uint64_t> g_mapEvents{0};

// Cached page size. Deliberately an atomic rather than a function-local
// static: this runs on the allocation path during static init, and a
// magic-static guard is one more thing that has to be alive down here.
// The race is benign — both racers compute the same value.
std::atomic<size_t> g_pageSize{0};

inline size_t pageSize() {
    size_t s = g_pageSize.load(std::memory_order_relaxed);
    if (s == 0) {
#if defined(_WIN32)
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        s = (size_t)si.dwPageSize;
#else
        s = (size_t)::getpagesize();
#endif
        g_pageSize.store(s, std::memory_order_relaxed);
    }
    return s;
}

// A 2MB-aligned region. `base` is what the allocator uses; `raw` is what has
// to be handed back to the OS.
//
// They differ on Windows. mmap can unmap a sub-range, so POSIX over-allocates
// and trims the head/tail, leaving base == raw. VirtualFree(MEM_RELEASE) can
// only release an entire reservation, never part of one, so the same trick is
// impossible: Windows instead RESERVES the oversized span, COMMITS only the
// aligned window inside it, and remembers the reservation base for release.
// The untouched head/tail cost address space but no physical memory — a
// non-issue in a 64-bit address space.
struct Mapping {
    void* base;
    void* raw;
};

// size must be page-aligned. Returns {nullptr, nullptr} on failure.
Mapping mapAligned(size_t size) {
    const size_t over = size + kBlockSize;

#if defined(_WIN32)
    void* raw = ::VirtualAlloc(nullptr, over, MEM_RESERVE, PAGE_READWRITE);
    if (!raw) return {nullptr, nullptr};
    const uintptr_t base = ((uintptr_t)raw + kBlockSize - 1) & kBlockMask;
    if (!::VirtualAlloc((void*)base, size, MEM_COMMIT, PAGE_READWRITE)) {
        ::VirtualFree(raw, 0, MEM_RELEASE);
        return {nullptr, nullptr};
    }
#else
    void* raw = ::mmap(nullptr, over, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return {nullptr, nullptr};
    const uintptr_t base = ((uintptr_t)raw + kBlockSize - 1) & kBlockMask;
    const size_t head = base - (uintptr_t)raw;
    const size_t tail = over - head - size;
    if (head) ::munmap(raw, head);
    if (tail) ::munmap((void*)(base + size), tail);
    raw = (void*)base;   // head/tail are gone; the region IS the mapping
#endif

    g_mappedBytes.fetch_add(size, std::memory_order_relaxed);
    g_mapEvents.fetch_add(1, std::memory_order_relaxed);
    return {(void*)base, raw};
}

// `bytes` is the committed size on POSIX and ignored on Windows, where
// MEM_RELEASE requires a zero length and frees the whole reservation.
void unmapRegion(void* raw, size_t bytes) {
#if defined(_WIN32)
    (void)bytes;
    ::VirtualFree(raw, 0, MEM_RELEASE);
#else
    ::munmap(raw, bytes);
#endif
}

// ── Immortal locking ────────────────────────────────────────────────────────
// The manager must outlive STATIC DESTRUCTION: destructors of other statics
// allocate/free through the routed global new/delete after main() returns.
// std::mutex has a real destructor — plain static TagHeaps therefore died at
// exit and the next lock() threw from libc++ (the cmd+q SIGSEGV). POSIX
// mutexes with PTHREAD_MUTEX_INITIALIZER are trivially destructible POD:
// "destroying" them is a no-op, so post-exit allocations keep working.
//
// SRWLOCK is the Windows equivalent and has the same property: SRWLOCK_INIT
// is a zero-initializer, there is no Delete/Destroy call in the API at all,
// and the type is trivially destructible. The static_assert below is the
// real contract — any future swap must keep it.
#if defined(_WIN32)
using ImmortalMutex = SRWLOCK;
#  define MEM_MUTEX_INIT SRWLOCK_INIT
inline void mutexLock(ImmortalMutex& m)   { ::AcquireSRWLockExclusive(&m); }
inline void mutexUnlock(ImmortalMutex& m) { ::ReleaseSRWLockExclusive(&m); }
#else
using ImmortalMutex = pthread_mutex_t;
#  define MEM_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
inline void mutexLock(ImmortalMutex& m)   { ::pthread_mutex_lock(&m); }
inline void mutexUnlock(ImmortalMutex& m) { ::pthread_mutex_unlock(&m); }
#endif

static_assert(std::is_trivially_destructible_v<ImmortalMutex>,
              "the allocator's locks must survive static destruction");

struct ImmortalLock {
    explicit ImmortalLock(ImmortalMutex& m) : m_m(&m) { mutexLock(*m_m); }
    ~ImmortalLock() { mutexUnlock(*m_m); }
    ImmortalLock(const ImmortalLock&)            = delete;
    ImmortalLock& operator=(const ImmortalLock&) = delete;
    ImmortalMutex* m_m;
};

// ── Block registry — "is this pointer ours?" ────────────────────────────────
// Open-addressing table of live block bases. Inserts/erases are rare (heap
// growth, large alloc/free) and mutex'd; lookups are lock-free atomic probes
// on the free/realloc hot path. Sized for 64GB of live 2MB regions.
constexpr size_t    kRegSlots = size_t(1) << 15;
constexpr uintptr_t kTombstone = 1;
std::atomic<uintptr_t> g_registry[kRegSlots];   // zero-init: empty
ImmortalMutex          g_regMu = MEM_MUTEX_INIT;

inline size_t regHash(uintptr_t base) {
    return size_t(((base >> kBlockShift) * 0x9E3779B97F4A7C15ull) >> 49);
}

void regInsert(uintptr_t base) {
    ImmortalLock lk(g_regMu);
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
    ImmortalLock lk(g_regMu);
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

struct TagHeap;
inline TagHeap& heap(Tag t);

// ── Shard — one striped TLSF pool + its own lock ────────────────────────────
// Only mu (immortal, trivially destructible) + tlsf: both trivially constant-initializable, so
// g_heaps stays safe to touch from the first static-init allocation. The tag
// flows in per call (or from the block header) so no self-referential setup.
struct Shard {
    ImmortalMutex   mu   = MEM_MUTEX_INIT;
    tlsf_t          tlsf = nullptr;

    bool grow(Tag tag) {
        const Mapping m = mapAligned(kBlockSize);
        void* b = m.base;
        if (!b) return false;
        auto* h = (BlockHeader*)b;
        h->magic = kMagic; h->tag = (uint8_t)tag; h->isLarge = 0;
        h->mapBytes = kBlockSize; h->userBytes = 0; h->shard = this;
        h->mapRaw = m.raw;
        void* pool = (char*)b + kHeaderSize;
        if (!tlsf) tlsf = tlsf_create_with_pool(pool, kBlockSize - kHeaderSize);
        else       tlsf_add_pool(tlsf, pool, kBlockSize - kHeaderSize);
        regInsert((uintptr_t)b);
        return true;
    }
    void*  allocate(Tag tag, size_t size, size_t align);   // needs heap() → below
    void   release(Tag tag, void* p);
    size_t liveSize(void* p) { ImmortalLock lk(mu); return tlsf_block_size(p); }
};

// ── TagHeap — kShards striped pools + tag-level stats ───────────────────────
struct TagHeap {
    Shard                 shards[kShards];
    std::atomic<uint64_t> cur{0}, peak{0}, allocs{0}, frees{0};
    std::atomic<uint64_t> budget{0};
    std::atomic<bool>     budgetWarned{false};

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
    void drop(uint64_t sz) {
        cur.fetch_sub(sz, std::memory_order_relaxed);
        frees.fetch_add(1, std::memory_order_relaxed);
    }
};

// Constant-initialized: safe to touch from the first static-init allocation.
TagHeap g_heaps[(size_t)Tag::Count];
inline TagHeap& heap(Tag t) { return g_heaps[(size_t)t]; }

// Round-robin a shard to each thread once, so N threads spread across N locks.
std::atomic<uint32_t> g_shardCounter{0};
thread_local int      t_shard = -1;
inline int myShard() {
    if (t_shard < 0)
        t_shard = (int)(g_shardCounter.fetch_add(1, std::memory_order_relaxed) % kShards);
    return t_shard;
}

// Shard methods that touch tag-level stats — defined after heap()/TagHeap.
void* Shard::allocate(Tag tag, size_t size, size_t align) {
    ImmortalLock lk(mu);
    if (!tlsf && !grow(tag)) return nullptr;
    void* p = align <= 8 ? tlsf_malloc(tlsf, size) : tlsf_memalign(tlsf, align, size);
    if (!p) {   // fragmented shard: a fresh 2MB block always fits a <1MB request
        if (!grow(tag)) return nullptr;
        p = align <= 8 ? tlsf_malloc(tlsf, size) : tlsf_memalign(tlsf, align, size);
        if (!p) return nullptr;
    }
    heap(tag).bump(tlsf_block_size(p));
    return p;
}
void Shard::release(Tag tag, void* p) {
    uint64_t sz;
    { ImmortalLock lk(mu); sz = tlsf_block_size(p); tlsf_free(tlsf, p); }
    heap(tag).drop(sz);
}

// ── Thread-local tag scope stack (no allocation, fixed depth) ───────────────
constexpr int kScopeDepth = 16;
thread_local Tag t_scopes[kScopeDepth];
thread_local int t_scopeTop = 0;

// ── Large allocations — dedicated aligned mappings ──────────────────────────
void* largeAlloc(Tag tag, size_t size, size_t align) {
    if (align > kBlockSize / 2) return nullptr;   // nobody needs this
    const size_t offset = (kHeaderSize + align - 1) & ~(align - 1);
    const size_t page   = pageSize();
    const size_t total  = (offset + size + page - 1) & ~(page - 1);
    const Mapping m = mapAligned(total);
    void* base = m.base;
    if (!base) return nullptr;
    auto* h = (BlockHeader*)base;
    h->magic = kMagic; h->tag = (uint8_t)tag; h->isLarge = 1;
    h->mapBytes = total; h->userBytes = size; h->shard = nullptr;
    h->mapRaw = m.raw;
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
    // Read the reservation base BEFORE unmapping — the header lives inside
    // the region being released.
    void* raw = h->mapRaw;
    unmapRegion(raw, h->mapBytes);
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
    return heap(tag).shards[myShard()].allocate(tag, size, align);
}
void* alloc(size_t size, size_t align) {
    return alloc(size, align, currentTag());
}

void free(void* p) {
    if (!p) return;
    BlockHeader* h = headerOf(p);
    if (!h) { std::free(p); return; }   // foreign: pre-routing / kit-side new
    if (h->isLarge) largeFree(h);
    else            h->shard->release((Tag)h->tag, p);
}

void* realloc(void* p, size_t newSize) {
    if (!p) return alloc(newSize, 8, currentTag());
    if (newSize == 0) { free(p); return nullptr; }
    BlockHeader* h = headerOf(p);
    if (!h) return std::realloc(p, newSize);   // foreign provenance stays std
    const Tag tag = (Tag)h->tag;
    const size_t oldSize = h->isLarge ? h->userBytes : h->shard->liveSize(p);
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
    return h->isLarge ? h->userBytes : h->shard->liveSize(p);
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
