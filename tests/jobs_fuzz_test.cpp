// ── jobs_fuzz_test — random use of every entry point, checked exactly ───────
//
// Each seed builds a different mix of work and throws it at the pool from the
// main thread, from threads the engine does not own, and from inside jobs:
//   run() at each priority, kept and discarded handles, waits in any order;
//   parallelFor with random counts and grains, nested inside jobs and inside
//   other parallelFors; runIo(); onMain(); scratch memory with canaries.
//
// What must hold, for every seed, on every backend (the test names nothing a
// backend owns, only jobs.h):
//   * every job body ran EXACTLY once, and every parallelFor index exactly once
//   * a wait() returns only after its job finished
//   * scratch memory a job wrote is intact when that job reads it back, even
//     when it ran other jobs while it waited
//   * onMain callbacks run on the main thread, all of them
//   * nothing hangs: ctest's timeout is the detector, and the seed is printed
//     before each round so a hang names it
//   * a job submitted while every worker sleeps runs without anyone helping
//     (liveness(), below: the one property the rounds cannot see)
//
//   jobs_fuzz_test [rounds] [first-seed]      (default 60 rounds from seed 1)
#include "core/jobs/jobs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } } while (0)

// ── The watchdog: a hang anywhere is a failure that names its phase ──────────
// A lost wake-up or a lost notify does not crash, it hangs. Every phase calls
// progress() as it goes; if nothing moves for 20 s the test fails, says where,
// and exits (a stuck job would hang shutdown too). Found necessary when a
// planted lost notify slipped past the waiter check and hung a fuzz round.
static std::atomic<uint64_t>    g_progress{0};
static std::atomic<const char*> g_phase{"start"};
static void progress() { g_progress.fetch_add(1, std::memory_order_relaxed); }
static void setPhase(const char* what) { g_phase.store(what, std::memory_order_relaxed); progress(); }
static void startWatchdog() {
    std::thread([] {
        uint64_t last = g_progress.load();
        auto since = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint64_t now = g_progress.load(std::memory_order_relaxed);
            if (now != last) { last = now; since = std::chrono::steady_clock::now(); continue; }
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(20)) {
                std::printf("  FAIL  no progress for 20 s in: %s\n", g_phase.load());
                std::fflush(stdout);
                std::_Exit(1);
            }
        }
    }).detach();
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

struct Round {
    std::atomic<uint64_t> expectedJobs{0}, ranJobs{0};
    std::atomic<uint64_t> expectedItems{0}, ranItems{0};
    std::atomic<uint64_t> expectedMain{0}, ranMain{0};
    std::atomic<uint64_t> expectedIo{0}, ranIo{0};
    std::atomic<int>      scratchCorrupt{0}, doubleRun{0}, earlyWait{0}, offMain{0};
    std::thread::id       mainThread;
};

// Scratch with a canary: write a pattern, do something that may run OTHER jobs
// on this thread (a nested parallelFor), then read the pattern back.
static void scratchProbe(Round& r, Rng& rng, int depth);

static void pfor(Round& r, Rng& rng, int depth) {
    const uint32_t count = 1 + rng.below(depth == 0 ? 3000 : 200);
    const uint32_t grain = 1 + rng.below(64);
    const jobs::Priority pr = (jobs::Priority)rng.below(3);
    std::vector<std::atomic<uint8_t>> hit(count);
    for (auto& h : hit) h.store(0, std::memory_order_relaxed);
    r.expectedItems.fetch_add(count);
    const uint64_t seed = rng.next();
    jobs::parallelFor("fuzz.for", count, grain, [&](uint32_t b, uint32_t e) {
        Rng local(seed ^ b);
        for (uint32_t i = b; i < e; ++i) {
            if (hit[i].fetch_add(1, std::memory_order_relaxed) != 0) r.doubleRun.fetch_add(1);
            r.ranItems.fetch_add(1, std::memory_order_relaxed);
        }
        if (depth < 2 && local.below(16) == 0) scratchProbe(r, local, depth + 1);
    }, pr);
    for (uint32_t i = 0; i < count; ++i)
        if (hit[i].load(std::memory_order_relaxed) != 1) { r.doubleRun.fetch_add(1); break; }
}

static void scratchProbe(Round& r, Rng& rng, int depth) {
    jobs::ScratchScope scope;
    const size_t n = 16 + rng.below(depth == 0 ? 400000 : 4000);   // sometimes past the arena: overflow path
    const uint8_t tag = (uint8_t)(1 + rng.below(250));
    uint8_t* p = (uint8_t*)jobs::scratchAlloc(n, (size_t)1 << rng.below(7));
    if (!p) { r.scratchCorrupt.fetch_add(1); return; }
    std::memset(p, tag, n);
    if (depth < 2 && rng.below(3) == 0) pfor(r, rng, depth + 1);   // other jobs run on this thread here
    for (size_t i = 0; i < n; i += 97)
        if (p[i] != tag) { r.scratchCorrupt.fetch_add(1); break; }
    if (p[n - 1] != tag) r.scratchCorrupt.fetch_add(1);
}

// One actor: what one thread does in a round.
static void actor(Round& r, uint64_t seed, int ops) {
    Rng rng(seed);
    std::vector<jobs::JobHandle> held;
    std::vector<std::shared_ptr<std::atomic<bool>>> heldDone;
    for (int i = 0; i < ops; ++i) {
        switch (rng.below(10)) {
        case 0: case 1: case 2: case 3: {   // run, keep or discard the handle
            r.expectedJobs.fetch_add(1);
            auto done = std::make_shared<std::atomic<bool>>(false);
            const uint64_t s2 = rng.next();
            const bool nest = rng.below(12) == 0;
            jobs::JobHandle h = jobs::run("fuzz.run", [&r, done, s2, nest] {
                if (nest) { Rng inner(s2); if (inner.below(2)) pfor(r, inner, 1); else scratchProbe(r, inner, 1); }
                r.ranJobs.fetch_add(1, std::memory_order_relaxed);
                done->store(true, std::memory_order_release);
            }, (jobs::Priority)rng.below(3));
            if (rng.below(2)) { held.push_back(h); heldDone.push_back(done); }
            break;
        }
        case 4: case 5: pfor(r, rng, 0); break;
        case 6: scratchProbe(r, rng, 0); break;
        case 7: {                           // blocking lane
            r.expectedIo.fetch_add(1);
            auto done = std::make_shared<std::atomic<bool>>(false);
            jobs::JobHandle h = jobs::runIo("fuzz.io", [&r, done] {
                std::this_thread::yield();
                r.ranIo.fetch_add(1, std::memory_order_relaxed);
                done->store(true, std::memory_order_release);
            });
            if (rng.below(2)) { held.push_back(h); heldDone.push_back(done); }
            break;
        }
        case 8: {                           // main-thread channel
            r.expectedMain.fetch_add(1);
            jobs::onMain([&r] {
                if (std::this_thread::get_id() != r.mainThread) r.offMain.fetch_add(1);
                r.ranMain.fetch_add(1, std::memory_order_relaxed);
            });
            break;
        }
        case 9: {                           // wait on a random held handle, maybe twice
            if (held.empty()) break;
            const size_t k = rng.below((uint32_t)held.size());
            jobs::wait(held[k]);
            if (!heldDone[k]->load(std::memory_order_acquire)) r.earlyWait.fetch_add(1);
            if (rng.below(4) == 0) jobs::wait(held[k]);   // waiting again is legal
            break;
        }
        }
    }
    for (size_t k = 0; k < held.size(); ++k) {
        jobs::wait(held[k]);
        if (!heldDone[k]->load(std::memory_order_acquire)) r.earlyWait.fetch_add(1);
    }
}

static void runRound(uint64_t seed) {
    Round r;
    r.mainThread = std::this_thread::get_id();
    Rng rng(seed);
    const int externals = (int)rng.below(5);             // threads the engine did not start
    const int ops = 40 + (int)rng.below(160);
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < externals; ++t)
        threads.emplace_back([&r, &go, seed, t, ops] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            actor(r, seed * 1000 + (uint64_t)t + 1, ops);
        });
    go.store(true, std::memory_order_release);
    actor(r, seed * 1000, ops);
    for (auto& th : threads) th.join();

    // Discarded handles: nothing waits on those jobs, so drain by pumping until
    // the counts arrive. A real hang is ctest's timeout.
    for (int spin = 0; r.ranJobs.load() < r.expectedJobs.load() || r.ranIo.load() < r.expectedIo.load() ||
                       r.ranMain.load() < r.expectedMain.load(); ++spin) {
        jobs::pumpMain();
        jobs::parallelFor("fuzz.drain", 64, 1, [](uint32_t, uint32_t) {});   // helps the pool
        if (spin > 200) std::this_thread::yield();
    }
    jobs::pumpMain();

    CHECK(r.ranJobs.load() == r.expectedJobs.load(), "seed %llu: %llu run() bodies ran, %llu submitted",
          (unsigned long long)seed, (unsigned long long)r.ranJobs.load(), (unsigned long long)r.expectedJobs.load());
    CHECK(r.ranItems.load() == r.expectedItems.load(), "seed %llu: %llu parallelFor items ran, %llu expected",
          (unsigned long long)seed, (unsigned long long)r.ranItems.load(), (unsigned long long)r.expectedItems.load());
    CHECK(r.ranIo.load() == r.expectedIo.load(), "seed %llu: I/O jobs %llu of %llu", (unsigned long long)seed,
          (unsigned long long)r.ranIo.load(), (unsigned long long)r.expectedIo.load());
    CHECK(r.ranMain.load() == r.expectedMain.load(), "seed %llu: onMain %llu of %llu", (unsigned long long)seed,
          (unsigned long long)r.ranMain.load(), (unsigned long long)r.expectedMain.load());
    CHECK(r.doubleRun.load() == 0, "seed %llu: a parallelFor index ran twice or not at all", (unsigned long long)seed);
    CHECK(r.earlyWait.load() == 0, "seed %llu: wait() returned before its job finished", (unsigned long long)seed);
    CHECK(r.scratchCorrupt.load() == 0, "seed %llu: scratch memory was overwritten under a job", (unsigned long long)seed);
    CHECK(r.offMain.load() == 0, "seed %llu: an onMain callback ran off the main thread", (unsigned long long)seed);
}

// ── Liveness: a sleeping worker must wake by itself ─────────────────────────
// The rounds above cannot see a lost wake-up: a thread that waits also helps,
// so the work gets done anyway and only parallelism is lost. Here nobody
// helps. Workers are told to sleep the moment they are idle (spin 0), one job
// is submitted, and the submitter only watches a flag. If the wake-up is lost
// the job never runs. Many rounds, because the window is a few instructions.
// A thread parked in wait() is woken by the job's completion, with no timer to
// rescue a missed notify (waiters used to wake every 100 µs to 2 ms). One worker
// takes the job (the main thread spins until it has started, so it cannot run
// it itself), then the main thread waits with spinning off, so it parks at
// once. A lost notify hangs; the watchdog turns that into a failure.
static void waiterLiveness(int rounds) {
    jobs::init(1, 0);
    setPhase("waiter liveness: a thread parked in wait() was never woken (a lost notify)");
    for (int i = 0; i < rounds; ++i) {
        progress();
        std::atomic<bool> started{false};
        const int spinNs = (i % 5) * 20000;   // 0 to 80 µs, so the finish lands anywhere around the park
        jobs::JobHandle h = jobs::run("fuzz.waiter", [&started, spinNs] {
            started.store(true, std::memory_order_release);
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::nanoseconds(spinNs)) {}
        });
        while (!started.load(std::memory_order_acquire)) {}
        jobs::wait(h);
    }
    jobs::shutdown();
}

static void liveness(uint32_t workers, int rounds) {
    jobs::init(workers, 0);
    setPhase("worker liveness");
    int lost = 0;
    for (int i = 0; i < rounds && !lost; ++i) {
        progress();
        std::atomic<bool> ran{false};
        jobs::run("fuzz.live", [&ran] { ran.store(true, std::memory_order_release); });
        const auto start = std::chrono::steady_clock::now();
        while (!ran.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) { ++lost; break; }
            // not jobs::wait: that would run the job here and hide the bug
        }
        if ((i & 7) == 0) std::this_thread::yield();   // vary how asleep the workers are
    }
    CHECK(lost == 0, "liveness: a job submitted to sleeping workers never ran (a lost wake-up)");
    if (lost) std::_Exit(1);   // the stuck job would hang shutdown
    jobs::shutdown();
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 60;
    const uint64_t first = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    std::printf("jobs_fuzz_test: backend %s, %d rounds from seed %llu\n", jobs::backendName(), rounds,
                (unsigned long long)first);
    startWatchdog();
    // One worker is the sharp case: with more, another sleeper takes the wake-up
    // and hides a lost one (found by planting the bug: three workers passed).
    liveness(1, rounds * 400);
    liveness(2, rounds * 100);
    waiterLiveness(rounds * 100);
    for (int i = 0; i < rounds; ++i) {
        const uint64_t seed = first + (uint64_t)i;
        // Pool shape is part of the fuzz: 1 worker, few, or the default, and
        // with or without the idle spin. Init and shutdown every round, which
        // also exercises starting a pool twice in one process.
        Rng shape(seed);
        // Named arrays, not compound literals: `(uint32_t[]){...}` is C99, a
        // GCC/Clang extension in C++, and MSVC refuses it (C4576, perf.yml).
        static const uint32_t kWorkers[] = {0, 1, 2, 3, 7};
        static const int      kSpins[]   = {-1, 0, 20};
        const uint32_t workers = kWorkers[shape.below(5)];
        const int spin = kSpins[shape.below(3)];
        std::printf("  seed %llu (workers %u, spin %d)\n", (unsigned long long)seed, workers, spin);
        setPhase("a fuzz round (a wait or a job that never finished)");
        jobs::init(workers, spin);
        runRound(seed);
        jobs::shutdown();
        if (g_failures) break;
    }
    std::printf(g_failures ? "jobs_fuzz_test: %d FAILURE(S)\n" : "jobs_fuzz_test: ALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
