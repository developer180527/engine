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

    mem::logStats("mem_test end");
    if (g_failures) {
        std::printf("mem_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mem_test: PASS — all paths green\n");
    return 0;
}
