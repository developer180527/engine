// ── jobs_test — engine::jobs facade contract gauntlet (audit C.5) ───────────
// Regression for the JobHandle use-after-free: run() used to hand back a raw
// pointer into g_inflight while pumpMain()'s sweep freed completed blocks —
// wait() on a handle stashed across a pump dereferenced freed memory. The
// handle now co-owns the block, so the stash-then-wait pattern must be safe.
// Run under the ASan lane to prove the UAF stays dead. Exits non-zero on
// first failure.
#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/jobs/jobs.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("jobs_test: facade contract gauntlet\n");

    jobs::init();
    CHECK(jobs::initialized(), "pool up (%u threads)", jobs::workerCount());

    // ── 1. Basic run + wait ───────────────────────────────────────────────
    {
        std::atomic<int> ran{0};
        jobs::JobHandle h = jobs::run("t.basic", [&] { ++ran; });
        jobs::wait(h);
        CHECK(ran.load() == 1, "run + wait executes the job");
    }

    // ── 2. THE C.5 REGRESSION: wait() on a handle stashed across sweeps ──
    // Complete a job, pump enough times to guarantee the sweep ran, then
    // wait on the stale handle. Pre-fix: heap-use-after-free (ASan-visible).
    {
        std::atomic<int> ran{0};
        jobs::JobHandle stash = jobs::run("t.stash", [&] { ++ran; });
        // Ensure completion before sweeping (wait helps, so this terminates).
        jobs::wait(stash);
        for (int i = 0; i < 8; ++i) jobs::pumpMain();   // sweep freed it pre-fix
        jobs::wait(stash);                               // must be safe + instant
        CHECK(ran.load() == 1, "wait() on a handle stashed across pumpMain sweeps");

        jobs::JobHandle copy = stash;                    // copies share the block
        jobs::wait(copy);
        CHECK(copy.valid(), "handle copies stay valid after sweeps");
    }

    // ── 2b. The callable lives in the job block, not in a std::function ───
    // Move-only captures work (std::function needed copyable ones), a capture
    // over the 64-byte inline size takes the heap path, and every capture is
    // destroyed exactly once, on the thread that ran the job.
    {
        struct Counted {
            std::atomic<int>* dtors;
            bool live = true;
            explicit Counted(std::atomic<int>* d) : dtors(d) {}
            Counted(Counted&& o) noexcept : dtors(o.dtors) { o.live = false; }
            ~Counted() { if (live) dtors->fetch_add(1); }
        };
        std::atomic<int> dtors{0}, ran{0};
        std::unique_ptr<int> owned(new int(41));
        jobs::JobHandle a = jobs::run("t.moveonly", [p = std::move(owned), c = Counted(&dtors), &ran] {
            ran.fetch_add(*p + 1 == 42 ? 1 : 0);
        });
        unsigned char big[200];
        for (int i = 0; i < 200; ++i) big[i] = (unsigned char)i;
        jobs::JobHandle b = jobs::run("t.big", [arr = std::to_array(big), c = Counted(&dtors), &ran] {
            int sum = 0;
            for (unsigned char v : arr) sum += v;
            ran.fetch_add(sum == 199 * 200 / 2 ? 1 : 0);
        });
        jobs::wait(a);
        jobs::wait(b);
        CHECK(ran.load() == 2, "a move-only capture and a 200-byte capture both run with their values");
        CHECK(dtors.load() == 2, "each capture destroyed exactly once when its job finished (%d)", dtors.load());
    }

    // ── 2c. frameBegin after a frame with no jobs wakes nobody ────────────
    // The idle-cost rule: an empty game must not pay for a pool it never uses.
    {
        jobs::frameBegin();   // takes in everything the tests above ran
        const uint64_t w0 = jobs::stats().wakes;
        for (int i = 0; i < 4; ++i) jobs::frameBegin();
        CHECK(jobs::stats().wakes == w0, "frameBegin after job-free frames asks the OS for no wake-up");
    }

    // ── 3. Default handle is 'already complete' ──────────────────────────
    {
        jobs::JobHandle none;
        jobs::wait(none);                                // must be a no-op
        CHECK(!none.valid(), "default handle waits as no-op");
    }

    // ── 4. parallelFor covers the whole range exactly once ───────────────
    {
        constexpr uint32_t N = 100000;
        std::atomic<uint64_t> sum{0};
        jobs::parallelFor("t.pfor", N, 512, [&](uint32_t b, uint32_t e) {
            uint64_t local = 0;
            for (uint32_t i = b; i < e; ++i) local += i;
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        const uint64_t expect = (uint64_t)N * (N - 1) / 2;
        CHECK(sum.load() == expect, "parallelFor sums [0,%u) exactly once", N);
    }

    // ── 5. onMain runs on pumpMain, in order ──────────────────────────────
    {
        int order = 0, first = 0, second = 0;
        jobs::onMain([&] { first  = ++order; });
        jobs::onMain([&] { second = ++order; });
        jobs::pumpMain();
        CHECK(first == 1 && second == 2, "onMain drains FIFO on pumpMain");
    }

    // ── 6. drainMain: the queue must be EMPTY before code is unloaded ────────
    // A kit's deferred callback is a function pointer into its dylib. The queue
    // used to be drained only by tickSystems' pumpMain(), while stopSimulation
    // dlclosed the kit — so anything queued after that frame's pump was invoked
    // next frame from unmapped memory. drainMain() is what runs immediately
    // before an unload, and one pumpMain() is NOT enough: pumpMain swaps the
    // queue before running it, so a callback that queues more work leaves that
    // work behind.
    {
        int ran = 0;
        // A chain three deep. One pumpMain() runs only the first link.
        jobs::onMain([&] {
            ++ran;
            jobs::onMain([&] {
                ++ran;
                jobs::onMain([&] { ++ran; });
            });
        });
        jobs::pumpMain();
        CHECK(ran == 1, "one pumpMain() runs ONE batch — the re-queued work "
              "is still pending (%d)", ran);
        jobs::drainMain();
        CHECK(ran == 3, "drainMain() keeps going until the queue is empty (%d)",
              ran);

        // A callback that re-queues itself FOREVER must not hang the shutdown it
        // is blocking. Bounded rounds, a warning, and the work is dropped —
        // dropping work is bad, calling into a freed library is worse.
        int spins = 0;
        std::function<void()> forever = [&] {
            ++spins;
            jobs::onMain(forever);
        };
        jobs::onMain(forever);
        jobs::drainMain(4);
        CHECK(spins == 4, "an unconditionally self-requeueing callback is capped "
              "at the round limit rather than spinning forever (%d)", spins);
        // And the pending entry is dropped, so a later pump cannot run it after
        // the unload this was protecting.
        const size_t after = jobs::pumpMain();
        CHECK(after == 0, "the runaway's last entry was dropped, not left queued "
              "for a pump after the dylib is gone (%zu)", after);
    }

    // ── 6. Handles outlive shutdown safely ────────────────────────────────
    jobs::JobHandle survivor = jobs::run("t.survivor", [] {});
    jobs::wait(survivor);
    jobs::shutdown();
    jobs::wait(survivor);                                // g_init gate → no-op
    CHECK(true, "stashed handle survives pool shutdown");

    if (g_failures) { std::printf("jobs_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("jobs_test: ALL PASS\n");
    return 0;
}
