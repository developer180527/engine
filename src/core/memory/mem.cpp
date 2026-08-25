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
// Guarded: the top-level CMakeLists already defines both for MSVC
// (add_compile_definitions), and an unguarded #define here is
// "warning C4005: macro redefinition" on every Windows TU that includes
// this. Harmless individually, and collectively it buries the warnings
// that mean something.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
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

// Live entries and tombstones, for the saturation guard below. Only mutated
// under g_regMu; read without it for reporting, so relaxed is right.
std::atomic<size_t> g_regLive{0}, g_regTombs{0};
std::atomic<bool>   g_regSatWarned{false};

// BOUNDED, because the unbounded version was an infinite loop the moment the
// table filled — and it spun holding g_regMu, so every subsequent alloc and free
// that touches the registry blocked behind it. A hang inside the allocator is the
// worst shape this failure can take: no crash, no stack that names the cause.
// Failing the insert instead turns 64 GB of live blocks into a refused allocation
// with a message.
bool regInsert(uintptr_t base) {
    ImmortalLock lk(g_regMu);
    size_t i = regHash(base);
    for (size_t probes = 0; probes < kRegSlots; ++probes) {
        uintptr_t v = g_registry[i].load(std::memory_order_relaxed);
        if (v == 0 || v == kTombstone) {
            if (v == kTombstone) g_regTombs.fetch_sub(1, std::memory_order_relaxed);
            g_registry[i].store(base, std::memory_order_release);
            g_regLive.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        i = (i + 1) & (kRegSlots - 1);
    }
    std::fprintf(stderr, "[mem] block registry FULL (%zu slots, all live) — "
                 "refusing to register %p. Allocation will fail; this is a %zu GB "
                 "live-block ceiling, not a leak in the table.\n",
                 kRegSlots, (void*)base, (kRegSlots * kBlockSize) >> 30);
    return false;
}

void regErase(uintptr_t base) {
    ImmortalLock lk(g_regMu);
    size_t i = regHash(base);
    for (size_t probes = 0; probes < kRegSlots; ++probes) {
        uintptr_t v = g_registry[i].load(std::memory_order_relaxed);
        if (v == base) {
            // ── Write 0 rather than a tombstone WHEN THE NEXT SLOT IS 0 ──────
            // Tombstones exist so a probe does not stop early at a deleted slot,
            // and nothing ever cleaned them up: `regHas` only stops at 0, so as
            // tombstones filled the table every MISS walked further, toward a
            // full 32 768-slot scan. Misses are the foreign-pointer path in
            // free()/realloc() — the hot path in exactly the mixed-allocator
            // case this design exists to support.
            //
            // Clearing to 0 is safe precisely when the following slot is already
            // 0: any key whose probe chain passes through here would have had to
            // continue past that 0 to be inserted, which cannot have happened. So
            // no present key becomes unreachable. Isolated entries — the common
            // case for large allocations — cost no tombstone at all.
            const size_t next = (i + 1) & (kRegSlots - 1);
            const bool isolated =
                g_registry[next].load(std::memory_order_relaxed) == 0;
            g_registry[i].store(isolated ? 0 : kTombstone,
                                std::memory_order_release);
            g_regLive.fetch_sub(1, std::memory_order_relaxed);
            if (!isolated) {
                const size_t t = g_regTombs.fetch_add(1, std::memory_order_relaxed) + 1;
                // A slope, not a cliff: probe length grows as the zero slots run
                // out. Reported once so it is a number somebody can see rather
                // than a mystery slowdown deep in free(). If this ever fires, the
                // fix is a double-buffered rebuild (a table swap is safe for the
                // lock-free readers; an in-place rehash is NOT).
                if (t + g_regLive.load(std::memory_order_relaxed) > kRegSlots * 3 / 4
                    && !g_regSatWarned.exchange(true))
                    std::fprintf(stderr, "[mem] block registry is %zu%% saturated "
                                 "(%zu live + %zu tombstones of %zu) — pointer "
                                 "provenance lookups are getting longer. See "
                                 "mem::logStats.\n",
                                 (t + g_regLive.load()) * 100 / kRegSlots,
                                 g_regLive.load(), t, kRegSlots);
            }
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
        // REGISTER BEFORE PUBLISHING TO TLSF. If the registry refuses, a pool
        // added here would hand out pointers that headerOf() cannot resolve —
        // free() would route them to std::free() and corrupt the system heap.
        // Better to drop the mapping and fail the growth.
        if (!regInsert((uintptr_t)b)) {
            g_mappedBytes.fetch_sub(kBlockSize, std::memory_order_relaxed);
            unmapRegion(m.raw, kBlockSize);
            return false;
        }
        void* pool = (char*)b + kHeaderSize;
        if (!tlsf) tlsf = tlsf_create_with_pool(pool, kBlockSize - kHeaderSize);
        else       tlsf_add_pool(tlsf, pool, kBlockSize - kHeaderSize);
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
    // A block changed size in place — `cur` moves, the alloc/free counts must
    // NOT. Using bump()/drop() here would invent an allocation and a free that
    // never happened and make the counts useless for churn analysis.
    void resized(uint64_t oldSz, uint64_t newSz) {
        if (newSz == oldSz) return;
        if (newSz < oldSz) { cur.fetch_sub(oldSz - newSz, std::memory_order_relaxed); return; }
        const uint64_t c = cur.fetch_add(newSz - oldSz, std::memory_order_relaxed)
                         + (newSz - oldSz);
        uint64_t pk = peak.load(std::memory_order_relaxed);
        while (c > pk && !peak.compare_exchange_weak(pk, c)) {}
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
// Pushes that did not fit. pop() unwinds these BEFORE touching the real stack.
thread_local int t_scopeDropped = 0;

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
    if (!regInsert((uintptr_t)base)) {   // see Shard::grow for why this is fatal
        g_mappedBytes.fetch_sub(total, std::memory_order_relaxed);
        unmapRegion(m.raw, total);
        return nullptr;
    }
    heap(tag).bump(size);
    return (char*)base + offset;
}

void largeFree(BlockHeader* h) {
    TagHeap& th = heap((Tag)h->tag);
    // drop(), not a hand-rolled fetch_sub pair: bump/drop are documented as the
    // only two places `cur` moves, and this copy was equivalent only by accident.
    // The first time drop() gains a step, the hand-rolled version silently stops
    // matching it.
    th.drop(h->userBytes);
    g_mappedBytes.fetch_sub(h->mapBytes, std::memory_order_relaxed);
    regErase((uintptr_t)h);
    // Read the reservation base BEFORE unmapping — the header lives inside
    // the region being released.
    void* raw = h->mapRaw;
    unmapRegion(raw, h->mapBytes);
}

inline BlockHeader* headerOf(void* p) {
    const uintptr_t base = (uintptr_t)p & kBlockMask;
    if (!regHas(base)) return nullptr;
    auto* h = (BlockHeader*)base;
    // ── magic was written and never read ────────────────────────────────────
    // Provenance came entirely from regHas(base), so a SCRIBBLED header was
    // trusted completely: `h->shard->release(...)` is a call through whatever
    // those eight bytes now contain, and `h->tag` indexes g_heaps.
    //
    // Aborting is the right answer and the alternatives are worse. Returning
    // null routes our own pointer to std::free(); carrying on makes a wild call.
    // Both are undefined and neither names the cause. The block base is printed
    // because it is the one thing that identifies which mapping was overwritten.
    if (h->magic != kMagic) {
        std::fprintf(stderr, "[mem] CORRUPT block header at %p (magic 0x%08x, "
                     "expected 0x%08x) while resolving %p. Something wrote over "
                     "the first %zu bytes of a 2MB block — a buffer overrun into "
                     "the block below, or a misaligned large allocation.\n",
                     (void*)base, h->magic, kMagic, p, kHeaderSize);
        std::fflush(stderr);
        std::abort();
    }
    return h;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────
const char* tagName(Tag t) {
    return (size_t)t < (size_t)Tag::Count ? kTagNames[(size_t)t] : "?";
}

Tag currentTag() { return t_scopeTop ? t_scopes[t_scopeTop - 1] : Tag::Core; }
// ── Overflow must stay BALANCED, not merely be dropped ──────────────────────
// A push past kScopeDepth used to vanish while the matching pop still
// decremented — so the pop consumed an OUTER scope's frame, and every
// allocation in that enclosing scope was attributed to the wrong tag from then
// on. The imbalance propagated outward and the final pop clamped at zero,
// hiding it. Counting the dropped pushes makes pop() unwind them first, so the
// frames that DID fit are never disturbed.
void pushTag(Tag t) {
    if (t_scopeTop < kScopeDepth) { t_scopes[t_scopeTop++] = t; return; }
    ++t_scopeDropped;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        std::fprintf(stderr, "[mem] MEM_SCOPE nested deeper than %d — tag '%s' is "
                     "not being attributed. Outer scopes stay correct; this one "
                     "charges to its parent.\n", kScopeDepth, tagName(t));
}
void popTag() {
    if (t_scopeDropped > 0) { --t_scopeDropped; return; }
    if (t_scopeTop > 0) --t_scopeTop;
}

void* alloc(size_t size, size_t align, Tag tag) {
    if (size == 0) size = 1;
    // ── align MUST be a power of two, and nothing checked ───────────────────
    // Every mask in this file — kBlockMask, largeAlloc's header offset,
    // tlsf_memalign's own contract — is only correct for a power of two. An
    // align of 24 produces a garbage mask, and the resulting offset is neither
    // aligned nor guaranteed to clear the BlockHeader, so the caller can be
    // handed a pointer that overlaps the header it will later be resolved
    // through. Reachable from OUTSIDE the engine: engineMemAlloc forwards a
    // kit's align straight here.
    if (align != 0 && (align & (align - 1)) != 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            std::fprintf(stderr, "[mem] alloc() refused: align=%zu is not a power "
                         "of two. Every provenance mask in the allocator assumes "
                         "it is; a bad value hands back a pointer overlapping its "
                         "own block header.\n", align);
        return nullptr;
    }
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
    if (newSize <= oldSize) {
        // ── A shrink in place still has to be ACCOUNTED for ─────────────────
        // Only for LARGE blocks. A pool block is accounted at
        // tlsf_block_size(p) on both bump and drop, and shrinking inside the
        // same TLSF block does not change it — so `cur` there is already right,
        // and touching it would make it wrong.
        //
        // Large blocks are accounted at the REQUESTED size, and this path
        // returned without updating `userBytes` — so stats over-reported until
        // the eventual free (which then subtracted the stale, larger value and
        // balanced out), and allocSize() reported more than the caller now owns.
        // The mapping is untouched, so this was never a safety bug; it made the
        // per-tag numbers lie, which for a telemetry-driven budget is enough.
        if (h->isLarge && newSize < oldSize) {
            heap(tag).resized(oldSize, newSize);
            h->userBytes = newSize;
        }
        return p;
    }
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
