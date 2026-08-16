// ── mem_test — memory manager regression gauntlet ───────────────────────────
// Exercises every path in mem:: (core/memory/): TLSF small allocs, large
// direct mappings, alignment, MEM_SCOPE attribution, realloc semantics,
// FOREIGN pointer fallback (malloc'd memory through mem::free — the kit/
// pre-routing safety net), cross-thread free, and a churn loop that must
// return to baseline with ZERO heap growth (the syscall-minimization
// invariant). Exits non-zero on the first failure.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/memory/mem.h"

static int  g_failures = 0;

// ── Post-main gauntlet ──────────────────────────────────────────────────────
// Static destructors run AFTER main() and may allocate/free through the
// routed global new/delete — the cmd+q crash class: heaps with std::mutex
// members died at static destruction and the next lock() threw. With the
// immortal (trivially-destructible) locks this must work; if it regresses,
// this destructor crashes the process and the harness sees a bad exit code.
struct PostMainAllocProbe {
    ~PostMainAllocProbe() {
        volatile int* p = new int[256];
        p[0] = 42; p[255] = 7;
        delete[] p;
        std::printf("  ok    post-main static-destructor allocation survives\n");
    }
} g_postMainProbe;
#define CHECK(cond, ...) do {                                        \
    if (!(cond)) {                                                   \
        std::printf("  FAIL  " __VA_ARGS__);                         \
        std::printf("  (%s:%d)\n", __FILE__, __LINE__);              \
        ++g_failures;                                                \
    } else {                                                         \
        std::printf("  ok    " __VA_ARGS__);                         \
        std::printf("\n");                                           \
    }                                                                \
} while (0)

int main() {
    std::printf("mem_test: memory manager gauntlet\n");

    // ── Basic alloc/free + ownership ────────────────────────────────────────
    {
        void* p = mem::alloc(128, 16, mem::Tag::Core);
        CHECK(p != nullptr, "small alloc returns memory");
        CHECK(mem::owns(p), "small alloc is registry-owned");
        CHECK(mem::allocSize(p) >= 128, "allocSize covers request");
        std::memset(p, 0xAB, 128);
        mem::free(p);
    }

    // ── Alignment (TLSF memalign + over-aligned large) ──────────────────────
    {
        for (size_t a : {8u, 16u, 64u, 256u, 4096u}) {
            void* p = mem::alloc(100, a, mem::Tag::Core);
            CHECK(((uintptr_t)p & (a - 1)) == 0, "alignment %zu honored", a);
            mem::free(p);
        }
    }

    // ── MEM_SCOPE attribution ───────────────────────────────────────────────
    {
        const uint64_t before = mem::stats(mem::Tag::Assets).currentBytes;
        void* p;
        {
            MEM_SCOPE(mem::Tag::Assets);
            CHECK(mem::currentTag() == mem::Tag::Assets, "scope tag active");
            p = mem::alloc(10000, 16);   // no explicit tag: reads the scope
        }
        CHECK(mem::currentTag() == mem::Tag::Core, "scope pops to Core");
        const uint64_t during = mem::stats(mem::Tag::Assets).currentBytes;
        CHECK(during >= before + 10000, "scoped alloc lands in Assets heap");
        mem::free(p);
        CHECK(mem::stats(mem::Tag::Assets).currentBytes == before,
              "Assets returns to baseline after free");
    }

    // ── Large allocation (direct 2MB-aligned mapping) ───────────────────────
    {
        const uint64_t mapped0 = mem::mappedBytes();
        void* p = mem::alloc(8 * 1024 * 1024, 16, mem::Tag::Assets);
        CHECK(p != nullptr, "8MB large alloc");
        CHECK(mem::owns(p), "large alloc is registry-owned");
        CHECK(mem::allocSize(p) == 8 * 1024 * 1024, "large allocSize exact");
        std::memset(p, 0xCD, 8 * 1024 * 1024);   // touch every page
        CHECK(mem::mappedBytes() > mapped0, "large alloc mapped new memory");
        mem::free(p);
        CHECK(mem::mappedBytes() == mapped0, "large free returns mapping to OS");
    }

    // ── Realloc: grow preserves contents, shrink keeps in place ─────────────
    {
        char* p = (char*)mem::alloc(64, 8, mem::Tag::Core);
        std::memcpy(p, "0123456789", 10);
        char* q = (char*)mem::realloc(p, 300000);   // grow (stays sub-large)
        CHECK(q && std::memcmp(q, "0123456789", 10) == 0,
              "realloc grow preserves contents");
        char* r = (char*)mem::realloc(q, 32);
        CHECK(r == q, "realloc shrink keeps the block in place");
        mem::free(r);
    }

    // ── Foreign pointers: the kit / pre-routing safety net ──────────────────
    {
        void* m = std::malloc(64);
        CHECK(!mem::owns(m), "malloc'd pointer is foreign");
        mem::free(m);   // must forward to std::free, not crash
        CHECK(true, "foreign free forwards to std::free");
        char* s = (char*)std::malloc(16);
        std::memcpy(s, "hello", 6);
        s = (char*)mem::realloc(s, 4096);
        CHECK(s && std::memcmp(s, "hello", 6) == 0,
              "foreign realloc forwards to std::realloc");
        std::free(s);
    }

    // ── Cross-thread: alloc on worker, free on main (and reverse) ───────────
    {
        void* fromWorker = nullptr;
        void* toWorker   = mem::alloc(512, 16, mem::Tag::Jobs);
        std::thread t([&] {
            fromWorker = mem::alloc(512, 16, mem::Tag::Jobs);
            mem::free(toWorker);
        });
        t.join();
        CHECK(fromWorker && mem::owns(fromWorker), "worker-thread alloc owned");
        mem::free(fromWorker);
        CHECK(true, "cross-thread alloc/free both directions");
    }

    // ── Churn: steady state must not grow the heap (syscall invariant) ──────
    {
        // Warm the Core heap with the worst case of the loop below…
        std::vector<void*> warm;
        for (int i = 0; i < 512; ++i)
            warm.push_back(mem::alloc(1024 + (i * 37) % 4096, 16, mem::Tag::Core));
        for (void* p : warm) mem::free(p);

        const uint64_t maps0 = mem::mapEventCount();
        const uint64_t live0 = mem::stats(mem::Tag::Core).currentBytes;
        for (int frame = 0; frame < 1000; ++frame) {
            void* ptrs[64];
            for (int i = 0; i < 64; ++i)
                ptrs[i] = mem::alloc(64 + (i * 97 + frame * 13) % 2048, 16,
                                     mem::Tag::Core);
            for (int i = 0; i < 64; ++i) mem::free(ptrs[i]);
        }
        CHECK(mem::mapEventCount() == maps0,
              "64k-alloc churn: ZERO heap growth (no syscalls)");
        CHECK(mem::stats(mem::Tag::Core).currentBytes == live0,
              "churn returns to live baseline");
    }

    // ── Budgets: soft warning fires (visually) without failing ──────────────
    {
        mem::setBudget(mem::Tag::Audio, 1024);
        void* p = mem::alloc(4096, 16, mem::Tag::Audio);   // exceeds → warns
        mem::free(p);
        mem::setBudget(mem::Tag::Audio, 0);
        CHECK(true, "budget warning path exercised (see [mem] line above)");
    }

    // ── Global new routing (this binary links the routed overrides) ─────────
    {
        auto* v = new std::vector<int>();
        v->resize(100000);
        CHECK(mem::owns(v->data()), "std::vector storage routed to mem::");
        delete v;
    }

    // ── A shrinking LARGE realloc must correct the accounting ───────────────
    // Large blocks are charged at the REQUESTED size, and the in-place shrink
    // path used to return without updating it — so stats over-reported until the
    // free, and allocSize() claimed more than the caller owned. Pool blocks are
    // charged at tlsf_block_size() and must NOT change, so both are asserted:
    // the fix has to touch one and leave the other alone.
    {
        const uint64_t live0 = mem::stats(mem::Tag::Core).currentBytes;
        void* big = mem::alloc(4u << 20, 8, mem::Tag::Core);        // large: 4 MB
        CHECK(big && mem::allocSize(big) == (4u << 20),
              "a 4 MB block reports its requested size (%zu)", mem::allocSize(big));
        const uint64_t liveBig = mem::stats(mem::Tag::Core).currentBytes;
        CHECK(liveBig >= live0 + (4u << 20), "and is charged to the tag");

        void* small = mem::realloc(big, 1u << 20);                 // shrink to 1 MB
        CHECK(small == big, "a large shrink keeps the block in place");
        CHECK(mem::allocSize(small) == (1u << 20),
              "allocSize follows the shrink (%zu, expected %u)",
              mem::allocSize(small), 1u << 20);
        CHECK(mem::stats(mem::Tag::Core).currentBytes <= liveBig - (3u << 20),
              "and the tag is credited the 3 MB given back (%llu -> %llu)",
              (unsigned long long)liveBig,
              (unsigned long long)mem::stats(mem::Tag::Core).currentBytes);
        mem::free(small);
        CHECK(mem::stats(mem::Tag::Core).currentBytes == live0,
              "a shrink then free balances exactly, with no double-credit (%llu vs %llu)",
              (unsigned long long)mem::stats(mem::Tag::Core).currentBytes,
              (unsigned long long)live0);

        // The pool case, which must be left alone: same block, same accounting.
        const uint64_t poolBefore = mem::stats(mem::Tag::Core).currentBytes;
        void* a = mem::alloc(4096, 8, mem::Tag::Core);
        const uint64_t withA = mem::stats(mem::Tag::Core).currentBytes;
        void* b = mem::realloc(a, 64);
        CHECK(b == a && mem::stats(mem::Tag::Core).currentBytes == withA,
              "a POOL shrink changes nothing: TLSF still holds the same block");
        mem::free(b);
        CHECK(mem::stats(mem::Tag::Core).currentBytes == poolBefore,
              "and it still balances on free");
    }

    // ── A non-power-of-two alignment is REFUSED, not masked ─────────────────
    // Reachable from outside the engine: engineMemAlloc forwards a kit's align
    // straight through. The masks in the allocator are only correct for powers of
    // two, and a bad one can hand back a pointer overlapping its own header.
    {
        CHECK(mem::alloc(64, 24, mem::Tag::Core) == nullptr,
              "align=24 is refused");
        CHECK(mem::alloc(64, 3, mem::Tag::Core) == nullptr, "align=3 is refused");
        void* ok = mem::alloc(64, 64, mem::Tag::Core);
        CHECK(ok && ((uintptr_t)ok % 64) == 0,
              "...while a real power of two still works and is aligned");
        mem::free(ok);
    }

    // ── MEM_SCOPE deeper than the stack must not corrupt OUTER scopes ────────
    // A dropped push used to leave the matching pop consuming an outer frame, so
    // every allocation in the ENCLOSING scope was misattributed from then on —
    // the imbalance propagated outward and the last pop clamped at zero, hiding
    // it. Nest past the limit and assert the tag we come back to.
    {
        MEM_SCOPE(mem::Tag::Assets);
        CHECK(mem::currentTag() == mem::Tag::Assets, "outer scope is Assets");
        {
            // 40 levels against a 16-deep stack: 24 pushes get dropped.
            struct Nest {
                static void go(int depth) {
                    if (depth == 0) return;
                    MEM_SCOPE(mem::Tag::Physics);
                    go(depth - 1);
                }
            };
            Nest::go(40);
        }
        CHECK(mem::currentTag() == mem::Tag::Assets,
              "after 40 nested scopes unwind, the OUTER tag is intact (got '%s')",
              mem::tagName(mem::currentTag()));
        void* p = mem::alloc(128, 8);
        CHECK(mem::owns(p), "and allocation still works in the restored scope");
        mem::free(p);
    }

    mem::logStats("mem_test end");
    if (g_failures) {
        std::printf("mem_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mem_test: PASS — all paths green\n");
    return 0;
}
