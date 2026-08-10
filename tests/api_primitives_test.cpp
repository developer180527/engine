// ── api_primitives_test — the primitive tier of the engine API table ────────
// The table used to expose only the engine's SUBSYSTEMS: physics, animation,
// nav, audio. Those are finished opinions, and a developer who wants their own
// animation system cannot build one out of them — they can only adopt ours or
// fork the engine.
//
// jobs / memory / drawSubmit are the layer underneath: the worker pool, the
// tagged heaps and frame arena, and geometry submission that owns no entity.
// With them, our animator is the DEFAULT implementation rather than the only
// one.
//
// This asserts the contract a kit depends on, not the implementation:
//   • parallelFor covers [0,count) exactly once — no gaps, no double-visits,
//     whatever grain and worker count the host chose
//   • it is safe to call from several threads at once (the callback signature
//     promises this, so the test has to actually do it)
//   • tagged allocation is real memory, and the telemetry SEES it — the whole
//     reason for a kit not to use plain malloc
//   • frame-arena allocation is aligned and distinct
//   • an unknown tag is charged, not refused (a kit built against a newer tag
//     list must still run on an older host)
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <engine/engine_api.h>

#include "core/memory/mem.h"
#include "core/frame_arena.h"
#include "runtime/jobs/jobs.h"
#include "runtime/scripting/engine_api_binding.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

struct CoverCtx { std::atomic<int>* hits; uint32_t count; };

static void coverRange(void* user, uint32_t begin, uint32_t end) {
    auto* c = (CoverCtx*)user;
    for (uint32_t i = begin; i < end && i < c->count; ++i)
        c->hits[i].fetch_add(1, std::memory_order_relaxed);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("api_primitives_test: jobs / memory / drawSubmit\n");

    jobs::init();

    // ── jobs ────────────────────────────────────────────────────────────────
    CHECK(engineJobsWorkerCount() > 0,
          "the pool reports workers (%u)", engineJobsWorkerCount());

    // Sizes chosen to hit the EDGES of the partition, not to be big: 1 is the
    // degenerate range, 7 is smaller than any sensible grain, 1000 does not
    // divide evenly by 64. A first draft swept to 65 536 across 12 grain
    // combinations and turned a unit test into a stress run — coverage of the
    // partition logic needs awkward numbers, not large ones.
    for (uint32_t count : { 1u, 7u, 1000u }) {
        for (uint32_t grain : { 1u, 64u }) {
            std::vector<std::atomic<int>> hits(count);
            for (auto& h : hits) h.store(0, std::memory_order_relaxed);
            CoverCtx ctx{ hits.data(), count };
            engineJobsParallelFor("cover", count, grain, coverRange, &ctx);

            uint32_t missed = 0, doubled = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const int n = hits[i].load(std::memory_order_relaxed);
                if (n == 0) ++missed;
                else if (n > 1) ++doubled;
            }
            // A partition bug here is the worst kind: the frame still renders,
            // just with some entities never updated and others updated twice.
            CHECK(missed == 0 && doubled == 0,
                  "parallelFor covers [0,%u) exactly once at grain %u "
                  "(missed %u, doubled %u)", count, grain, missed, doubled);
        }
    }

    // Concurrent entry. The callback signature promises a kit may call this
    // from its own threads, so prove it rather than assume it.
    {
        constexpr uint32_t kN = 512;
        std::vector<std::atomic<int>> hits(kN);
        for (auto& h : hits) h.store(0, std::memory_order_relaxed);
        CoverCtx ctx{ hits.data(), kN };
        std::vector<std::thread> callers;
        constexpr int kCallers = 2;
        for (int t = 0; t < kCallers; ++t)
            callers.emplace_back([&]{
                engineJobsParallelFor("concurrent", kN, 32, coverRange, &ctx);
            });
        for (auto& t : callers) t.join();

        uint32_t wrong = 0;
        for (uint32_t i = 0; i < kN; ++i)
            if (hits[i].load(std::memory_order_relaxed) != kCallers) ++wrong;
        CHECK(wrong == 0,
              "%d concurrent parallelFor calls each cover every index once "
              "(%u indices with the wrong count)", kCallers, wrong);
    }

    // ── memory ──────────────────────────────────────────────────────────────
    {
        const uint64_t before = engineMemTaggedBytes((uint8_t)mem::Tag::Scripting);
        void* p = engineMemAlloc(64 * 1024, 16, (uint8_t)mem::Tag::Scripting);
        CHECK(p != nullptr, "tagged alloc returns memory");
        CHECK(((uintptr_t)p % 16) == 0, "...honouring the requested alignment");
        if (p) std::memset(p, 0xAB, 64 * 1024);   // it must be really writable

        const uint64_t during = engineMemTaggedBytes((uint8_t)mem::Tag::Scripting);
        CHECK(during >= before + 64 * 1024,
              "the TELEMETRY sees it (%llu -> %llu bytes) — the reason a kit "
              "should not use plain malloc",
              (unsigned long long)before, (unsigned long long)during);
        CHECK(engineMemAllocSize(p) >= 64 * 1024,
              "allocSize reports at least what was asked for (%zu)",
              engineMemAllocSize(p));

        engineMemFree(p);
        const uint64_t after = engineMemTaggedBytes((uint8_t)mem::Tag::Scripting);
        CHECK(after <= during - 64 * 1024, "and free gives it back (%llu)",
              (unsigned long long)after);
    }
    {
        // A kit compiled against a NEWER tag list must still run: an unknown
        // tag is charged to Core, never refused.
        void* p = engineMemAlloc(128, 8, 200);
        CHECK(p != nullptr, "an out-of-range tag still allocates (charged to Core)");
        engineMemFree(p);
    }

    // ── frame arena ─────────────────────────────────────────────────────────
    CHECK(engineMemFrameAlloc(64, 16) == nullptr,
          "frameAlloc returns null while UNBOUND — a documented outcome, so a "
          "kit written against it works in a host with no arena");
    {
        mem::FrameArena arena;
        arena.init(1 << 20);
        engineMemBindFrameArena(&arena);

        void* a = engineMemFrameAlloc(1024, 64);
        void* b = engineMemFrameAlloc(1024, 64);
        CHECK(a && b && a != b, "frameAlloc hands out distinct blocks");
        CHECK(a && ((uintptr_t)a % 64) == 0 && ((uintptr_t)b % 64) == 0,
              "...both aligned as asked");

        // Over-capacity must be REFUSED at the boundary. Deliberately only a
        // little over the 1 MB arena: the first draft asked for a terabyte and
        // the arena's heap-spill fallback happily tried to serve it, which is
        // what exposed the missing check in the first place — and pinned the
        // machine while doing it.
        CHECK(engineMemFrameAlloc((1 << 20) + 1, 16) == nullptr,
              "a request larger than the arena is refused, not spilled to the heap");
        CHECK(engineMemFrameAlloc(0, 16) == nullptr, "a zero-size request is null");

        engineMemBindFrameArena(nullptr);
        arena.shutdown();
    }

    // ── draw submission ─────────────────────────────────────────────────────
    // Unbound is the headless case, and it must be a silent no-op: the same kit
    // code runs on a dedicated server, where drawing nothing is correct.
    {
        const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        engineDrawSubmitMesh(1, 2, ident);
        CHECK(engineDrawSubmittedCount() == 0,
              "submitting with no renderer is a no-op, not a crash");
    }

    jobs::shutdown();

    if (g_failures) {
        std::printf("\napi_primitives_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\napi_primitives_test: PASS\n");
    return 0;
}
