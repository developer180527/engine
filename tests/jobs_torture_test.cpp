// ── jobs_torture_test — the job system at its edges ─────────────────────────
//
// jobs_fuzz_test checks random everyday use. This checks the edges, one case
// each, against jobs.h only:
//
//   huge        parallelFor over (nearly) 2^32 items: every index exactly once
//   overflow    1 000 000 run() calls with no waiting: the queue fills and the
//               submitter runs the rest; every job exactly once
//   recursive   fib(N) as a job tree (every node a run() and a wait()): deep
//               nested helping, ~N^φ jobs, on the workers' own stacks
//   nested      parallelFor inside parallelFor inside parallelFor, 4 deep
//   oversub     the pool busy while 3x as many CPU-hogging threads compete for
//               the cores: no hang, no lost job
//   foreign     2 000 short-lived threads, each submits and waits, then exits
//   churn       init / shutdown 200 times, with work in flight right up to it
//   latency     a job submitted to a parked pool: time until it starts
//               (p50, p99, max over 2 000 tries)
//   memory      10 000 000 jobs: the pool's live memory returns to where it
//               started, and does not grow with job count
//   priority    a saturated pool: Frame jobs submitted after Background ones
//               start before them
//   throw       a parallelFor body that throws on the CALLER's range: the
//               exception reaches the caller, and no queued ticket is left
//               pointing at its stack
//
//   jobs_torture_test [case ...]      no case: all of them; "quick": fewer reps
#include "core/jobs/jobs.h"
#include "core/memory/mem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static int  g_failures = 0;
static bool g_quick = false;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

static double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ── huge ────────────────────────────────────────────────────────────────────
// The range counter must not wrap: with uint32 arithmetic, threads still
// claiming after the end push it past 2^32 and claim ranges again from 0.
static void caseHuge() {
    jobs::init();
    for (uint32_t count : {0xFFFFFFFFu, 0xFFFFFF00u, 0x80000001u}) {
        std::atomic<uint64_t> covered{0}, calls{0}, bad{0};
        jobs::parallelFor("torture.huge", count, 1, [&](uint32_t b, uint32_t e) {
            if (e <= b || e > count) bad.fetch_add(1);
            covered.fetch_add((uint64_t)(e - b), std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);
        });
        CHECK(covered.load() == count && bad.load() == 0,
              "huge: parallelFor(%u) covered %llu items in %llu ranges, %llu bad ranges",
              count, (unsigned long long)covered.load(), (unsigned long long)calls.load(),
              (unsigned long long)bad.load());
    }
    jobs::shutdown();
}

// ── overflow ────────────────────────────────────────────────────────────────
static void caseOverflow() {
    jobs::init();
    const int N = g_quick ? 200000 : 1000000;
    std::vector<std::atomic<uint8_t>> ran(N);
    for (auto& r : ran) r.store(0, std::memory_order_relaxed);
    const jobs::Stats s0 = jobs::stats();
    const auto t = Clock::now();
    for (int i = 0; i < N; ++i)
        jobs::run("torture.flood", [&ran, i] { ran[i].fetch_add(1, std::memory_order_relaxed); });
    // Nothing waited on: drain by helping until the last one is seen.
    for (;;) {
        bool all = true;
        for (int i = N - 1; i >= 0 && all; i -= 997) all = ran[i].load() != 0;
        if (all) {
            int missing = 0;
            for (int i = 0; i < N; ++i) if (ran[i].load() == 0) { missing = 1; break; }
            if (!missing) break;
        }
        jobs::parallelFor("torture.help", 64, 1, [](uint32_t, uint32_t) {});
    }
    int twice = 0;
    for (auto& r : ran) if (r.load() != 1) ++twice;
    const jobs::Stats s1 = jobs::stats();
    CHECK(twice == 0, "overflow: %d jobs flooded in %.0f ms, each ran exactly once (%llu ran by the submitter)",
          N, msSince(t), (unsigned long long)(s1.inlineRuns - s0.inlineRuns));
    jobs::shutdown();
}

// ── recursive ───────────────────────────────────────────────────────────────
static uint64_t fib(int n, std::atomic<uint64_t>& jobs) {
    if (n < 2) return (uint64_t)n;
    uint64_t a = 0;
    jobs.fetch_add(1, std::memory_order_relaxed);
    jobs::JobHandle h = jobs::run("torture.fib", [&a, n, &jobs] { a = fib(n - 1, jobs); });
    const uint64_t b = fib(n - 2, jobs);
    jobs::wait(h);
    return a + b;
}
static void caseRecursive() {
    jobs::init();
    const int n = g_quick ? 22 : 27;
    std::atomic<uint64_t> count{0};
    uint64_t got = 0;
    const auto t = Clock::now();
    jobs::JobHandle h = jobs::run("torture.fibroot", [&] { got = fib(n, count); });
    jobs::wait(h);
    uint64_t want = 0, p = 1;
    for (int i = 1; i < n; ++i) { const uint64_t x = want + p; want = p; p = x; }
    want = p;
    CHECK(got == want, "recursive: fib(%d) = %llu as a tree of %llu nested jobs and waits, %.0f ms",
          n, (unsigned long long)got, (unsigned long long)count.load(), msSince(t));
    jobs::shutdown();
}

// ── nested ──────────────────────────────────────────────────────────────────
static void caseNested() {
    jobs::init();
    const uint32_t A = 16, B = 16, C = 16, D = g_quick ? 16 : 64;
    std::atomic<uint64_t> leaves{0};
    const auto t = Clock::now();
    jobs::parallelFor("n1", A, 1, [&](uint32_t b1, uint32_t e1) {
        for (uint32_t i = b1; i < e1; ++i)
            jobs::parallelFor("n2", B, 1, [&](uint32_t b2, uint32_t e2) {
                for (uint32_t j = b2; j < e2; ++j)
                    jobs::parallelFor("n3", C, 1, [&](uint32_t b3, uint32_t e3) {
                        for (uint32_t k = b3; k < e3; ++k)
                            jobs::parallelFor("n4", D, 4, [&](uint32_t b4, uint32_t e4) {
                                leaves.fetch_add(e4 - b4, std::memory_order_relaxed);
                            });
                    });
            });
    });
    CHECK(leaves.load() == (uint64_t)A * B * C * D, "nested: 4-deep parallelFor, %llu leaves of %llu, %.0f ms",
          (unsigned long long)leaves.load(), (unsigned long long)A * B * C * D, msSince(t));
    jobs::shutdown();
}

// ── oversub ─────────────────────────────────────────────────────────────────
static void caseOversub() {
    jobs::init();
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    std::atomic<bool> stop{false};
    std::vector<std::thread> hogs;
    for (unsigned i = 0; i < hw * 3; ++i)
        hogs.emplace_back([&stop] { volatile uint64_t x = 0; while (!stop.load(std::memory_order_relaxed)) x = x + 1; });
    const int rounds = g_quick ? 50 : 300;
    std::atomic<uint64_t> items{0}, small{0};
    const auto t = Clock::now();
    double worst = 0;
    for (int r = 0; r < rounds; ++r) {
        const auto f = Clock::now();
        std::vector<jobs::JobHandle> hs;
        for (int i = 0; i < 50; ++i) hs.push_back(jobs::run("o.run", [&small] { small.fetch_add(1); }));
        jobs::parallelFor("o.for", 10000, 64, [&](uint32_t b, uint32_t e) { items.fetch_add(e - b); });
        for (auto& h : hs) jobs::wait(h);
        worst = std::max(worst, msSince(f));
    }
    stop.store(true);
    for (auto& h : hogs) h.join();
    CHECK(items.load() == (uint64_t)rounds * 10000 && small.load() == (uint64_t)rounds * 50,
          "oversub: %d frames against %u CPU hogs on %u threads in %.0f ms, worst frame %.1f ms, nothing lost",
          rounds, hw * 3, hw, msSince(t), worst);
    jobs::shutdown();
}

// ── foreign ─────────────────────────────────────────────────────────────────
static void caseForeign() {
    jobs::init();
    const int N = g_quick ? 300 : 2000;
    std::atomic<int> done{0}, bad{0};
    const auto t = Clock::now();
    for (int batch = 0; batch < N; batch += 50) {
        std::vector<std::thread> ts;
        for (int i = 0; i < 50 && batch + i < N; ++i)
            ts.emplace_back([&] {
                std::atomic<int> x{0};
                jobs::JobHandle h = jobs::run("f.run", [&x] { x.fetch_add(1); });
                jobs::parallelFor("f.for", 100, 1, [&x](uint32_t b, uint32_t e) { x.fetch_add((int)(e - b)); });
                jobs::wait(h);
                if (x.load() != 101) bad.fetch_add(1);
                done.fetch_add(1);
            });
        for (auto& th : ts) th.join();
    }
    CHECK(done.load() == N && bad.load() == 0, "foreign: %d short-lived threads submitted and waited, %.0f ms",
          done.load(), msSince(t));
    jobs::shutdown();
}

// ── churn ───────────────────────────────────────────────────────────────────
static void caseChurn() {
    const int N = g_quick ? 40 : 200;
    std::atomic<uint64_t> ran{0}, want{0};
    const auto t = Clock::now();
    for (int i = 0; i < N; ++i) {
        jobs::init((uint32_t)(1 + i % 7), (i % 3) * 20);
        for (int k = 0; k < 200; ++k) {
            want.fetch_add(1);
            jobs::run("c.run", [&ran] { ran.fetch_add(1); });   // discarded handles: still queued at shutdown
        }
        jobs::shutdown();   // must run everything still queued
    }
    CHECK(ran.load() == want.load(), "churn: %d init/shutdown cycles, %llu of %llu queued jobs ran, %.0f ms",
          N, (unsigned long long)ran.load(), (unsigned long long)want.load(), msSince(t));
}

// ── latency ─────────────────────────────────────────────────────────────────
static void caseLatency() {
    jobs::init();
    const int N = g_quick ? 300 : 2000;
    std::vector<double> us;
    for (int i = 0; i < N; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));   // the pool parks
        std::atomic<int64_t> startedAt{0};
        const auto t0 = Clock::now();
        jobs::run("l.run", [&startedAt] { startedAt.store(Clock::now().time_since_epoch().count()); });
        while (startedAt.load() == 0) {}   // not wait(): that would run it here
        us.push_back(std::chrono::duration<double, std::micro>(Clock::time_point(Clock::duration(startedAt.load())) - t0).count());
    }
    std::sort(us.begin(), us.end());
    CHECK(us.back() < 50000.0, "latency: a job to a parked pool starts after p50 %.1f µs, p99 %.1f µs, max %.1f µs",
          us[us.size() / 2], us[us.size() * 99 / 100], us.back());
    jobs::shutdown();
}

// ── memory ──────────────────────────────────────────────────────────────────
static void caseMemory() {
    jobs::init();
    auto live = [] { return mem::stats(mem::Tag::Jobs).currentBytes; };
    auto burst = [](int n) {
        std::vector<jobs::JobHandle> hs;
        hs.reserve(1000);
        std::atomic<int> x{0};
        for (int i = 0; i < n; ++i) {
            hs.push_back(jobs::run("m.run", [&x] { x.fetch_add(1, std::memory_order_relaxed); }));
            if (hs.size() == 1000) { for (auto& h : hs) jobs::wait(h); hs.clear(); }
        }
        for (auto& h : hs) jobs::wait(h);
    };
    burst(100000);                         // warm the block caches
    const uint64_t warm = live();
    const int N = g_quick ? 1000000 : 10000000;
    const auto t = Clock::now();
    burst(N);
    const uint64_t after = live();
    CHECK(after <= warm + 64 * 1024, "memory: %d jobs, Jobs heap %llu B live after warm-up, %llu B after (%.0f ns/job)",
          N, (unsigned long long)warm, (unsigned long long)after, msSince(t) * 1e6 / N);
    jobs::shutdown();
}

// ── priority ────────────────────────────────────────────────────────────────
// A real inversion is a Background job starting while Frame jobs were still
// QUEUED. Frame jobs already taken by a thread but not yet started do not
// count: at most one per thread can be in that state, so a Background start
// is an inversion only when more Frame jobs had not started than there are
// threads.
static void casePriority() {
    jobs::init();
    const unsigned w = jobs::workerCount();
    const int rounds = g_quick ? 5 : 30;
    int worst = 0, inversions = 0;
    for (int r = 0; r < rounds; ++r) {
        std::atomic<bool> release{false};
        std::atomic<int> frameStarted{0}, maxWaiting{0}, inv{0};
        std::vector<jobs::JobHandle> hs;
        for (unsigned i = 0; i + 1 < w; ++i)   // occupy every worker, so what follows queues
            hs.push_back(jobs::run("p.block", [&release] { while (!release.load()) std::this_thread::yield(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const int N = 200;
        for (int i = 0; i < N; ++i)
            hs.push_back(jobs::run("p.bg", [&] {
                const int waiting = N - frameStarted.load();
                if (waiting > (int)w) inv.fetch_add(1);
                int m = maxWaiting.load();
                while (waiting > m && !maxWaiting.compare_exchange_weak(m, waiting)) {}
            }, jobs::Priority::Background));
        for (int i = 0; i < N; ++i)
            hs.push_back(jobs::run("p.frame", [&] { frameStarted.fetch_add(1); }, jobs::Priority::Frame));
        release.store(true);
        for (auto& h : hs) jobs::wait(h);
        worst = std::max(worst, maxWaiting.load());
        inversions += inv.load();
    }
    CHECK(inversions == 0, "priority: %d rounds, %d Background jobs started while Frame jobs were queued "
          "(most Frame jobs not yet started at a Background start: %d; %u threads may hold one each)",
          rounds, inversions, worst, w);
    jobs::shutdown();
}

// ── throw ───────────────────────────────────────────────────────────────────
static void caseThrow() {
    jobs::init();
    bool caught = false;
    std::atomic<uint64_t> ran{0};
    try {
        jobs::parallelFor("t.for", 100000, 16, [&](uint32_t b, uint32_t e) {
            ran.fetch_add(e - b);
            if (b == 0) throw std::runtime_error("body threw");   // index 0 is always the caller's first range
        });
    } catch (const std::runtime_error&) { caught = true; }
    // If tickets for the dead stack frame were left queued, this is where a
    // worker runs one: give them every chance.
    for (int i = 0; i < 50; ++i) jobs::parallelFor("t.after", 1000, 8, [](uint32_t, uint32_t) {});
    CHECK(caught, "throw: an exception from a parallelFor body on the caller's range reached the caller "
          "(%llu items had run), and the pool kept working", (unsigned long long)ran.load());
    jobs::shutdown();
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        {"huge", caseHuge}, {"overflow", caseOverflow}, {"recursive", caseRecursive}, {"nested", caseNested},
        {"oversub", caseOversub}, {"foreign", caseForeign}, {"churn", caseChurn}, {"latency", caseLatency},
        {"memory", caseMemory}, {"priority", casePriority}, {"throw", caseThrow},
    };
    std::vector<std::string> want;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "quick")) g_quick = true;
        else want.push_back(argv[i]);
    }
    std::printf("jobs_torture_test (%s)\n", g_quick ? "quick" : "full");
    for (const Case& c : cases) {
        if (!want.empty() && std::find(want.begin(), want.end(), c.name) == want.end()) continue;
        std::printf("[%s]\n", c.name);
        c.fn();
    }
    std::printf(g_failures ? "jobs_torture_test: %d FAILURE(S)\n" : "jobs_torture_test: ALL PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
