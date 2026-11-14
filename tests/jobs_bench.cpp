// ── jobs_bench — what the job system costs, in numbers (WO-078, DR-0036) ─────
//
// Not a pass/fail test: it prints one line per measurement, the median of
// several repeats, so a change to core/jobs is compared with the run before it.
// Build it optimised (the numbers from an unoptimised build mean nothing):
//
//   cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel --target jobs_bench
//   build-rel/jobs_bench
//
// What each line is:
//   run.dispatch      submit N empty jobs from one thread, then wait for all
//   run.dispatch.mt   the same from 4 threads at once (contended submission)
//   pfor.small        64 items of ~2.5 us each, grain 4: the smallest job worth splitting
//   pfor.scale        sum of 4M floats, parallelFor against the same loop serially
//   frame.mix         a frame's shape: 6 parallelFors (cull, extract, animate...)
//                     while 200 small jobs (physics) are in flight
//   allocs            global-new allocations made per run() job
//   frame.paced       frame.mix once every 4 ms, with the CPU the whole process used
//   idle              CPU used in half a second with nothing submitted
#include "core/jobs/jobs.h"
#include "core/mem_counters.h"
#include "core/memory/mem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
template <class F>
static double median(int repeats, F&& f) {
    std::vector<double> t;
    for (int i = 0; i < repeats; ++i) t.push_back(f());
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

int main(int argc, char** argv) {
    // jobs_bench [spinMicros]: how long an idle worker looks before it sleeps.
    const int spin = argc > 1 ? std::atoi(argv[1]) : -1;
    const std::clock_t cpu0 = std::clock();
    const auto wall0 = Clock::now();
    jobs::init(0, spin);
    const uint32_t threads = jobs::workerCount();
    std::printf("jobs_bench: %u task threads\n", threads);

    // ── run.dispatch ────────────────────────────────────────────────────────
    {
        const int N = 100000;
        std::vector<jobs::JobHandle> hs(N);
        std::atomic<int> ran{0};
        const double t = median(7, [&] {
            ran = 0;
            const auto a = Clock::now();
            for (int i = 0; i < N; ++i) hs[i] = jobs::run("bench", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
            for (int i = 0; i < N; ++i) jobs::wait(hs[i]);
            const auto b = Clock::now();
            for (auto& h : hs) h = {};
            jobs::pumpMain();
            return ms(a, b);
        });
        std::printf("run.dispatch      %8.1f ns/job   (%d jobs, ran %d)\n", t * 1e6 / N, N, ran.load());
    }
    // ── allocs per run ──────────────────────────────────────────────────────
    {
        const int N = 20000;
        std::vector<jobs::JobHandle> hs(N);
        std::atomic<int> ran{0};
        jobs::pumpMain();
        const uint64_t before = mem::stats(mem::Tag::Core).allocCount + mem::stats(mem::Tag::Jobs).allocCount;
        for (int i = 0; i < N; ++i) hs[i] = jobs::run("bench", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        for (int i = 0; i < N; ++i) jobs::wait(hs[i]);
        const uint64_t after = mem::stats(mem::Tag::Core).allocCount + mem::stats(mem::Tag::Jobs).allocCount;
        for (auto& h : hs) h = {};
        jobs::pumpMain();
        std::printf("allocs            %8.2f per run() job\n", double(after - before) / N);
    }
    // ── run.dispatch.mt ─────────────────────────────────────────────────────
    {
        const int T = 4, N = 25000;
        std::atomic<int> ran{0};
        const double t = median(5, [&] {
            ran = 0;
            const auto a = Clock::now();
            std::vector<std::thread> ts;
            for (int k = 0; k < T; ++k)
                ts.emplace_back([&] {
                    std::vector<jobs::JobHandle> hs(N);
                    for (int i = 0; i < N; ++i) hs[i] = jobs::run("bench", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
                    for (int i = 0; i < N; ++i) jobs::wait(hs[i]);
                });
            for (auto& th : ts) th.join();
            const auto b = Clock::now();
            jobs::pumpMain();
            return ms(a, b);
        });
        std::printf("run.dispatch.mt   %8.1f ns/job   (%d threads x %d, ran %d)\n", t * 1e6 / (T * N), T, N, ran.load());
    }
    // ── pfor.small ──────────────────────────────────────────────────────────
    // 64 items of ~2.5 us each, grain 4: ranges of ~10 us, which is the smallest
    // range jobs.h says is worth a task.
    {
        const int N = 5000;
        double out[64 * 8] = {};   // one cache line per item: no false sharing
        auto item = [&](uint32_t k) { double x = 0; for (int i = 0; i < 2500; ++i) x += std::sqrt((double)(i + k)); out[k * 8] = x; };
        const double serial = median(7, [&] { const auto a = Clock::now(); for (int i = 0; i < N; ++i) for (uint32_t k = 0; k < 64; ++k) item(k); return ms(a, Clock::now()); });
        const double par = median(7, [&] {
            const auto a = Clock::now();
            for (int i = 0; i < N; ++i)
                jobs::parallelFor("bench", 64, 4, [&](uint32_t b, uint32_t e) { for (uint32_t k = b; k < e; ++k) item(k); });
            return ms(a, Clock::now());
        });
        std::printf("pfor.small        %8.1f us/call parallel, %.1f us serial: %.1fx\n", par * 1e3 / N, serial * 1e3 / N, serial / par);
    }
    // ── pfor.scale ──────────────────────────────────────────────────────────
    {
        const uint32_t N = 4u << 20;
        std::vector<float> v(N);
        for (uint32_t i = 0; i < N; ++i) v[i] = float(i % 1000) * 0.001f;
        auto work = [&](uint32_t b, uint32_t e) { double s = 0; for (uint32_t i = b; i < e; ++i) s += std::sqrt(v[i]); return s; };
        volatile double keep = 0;
        const double serial = median(7, [&] { const auto a = Clock::now(); keep = work(0, N); return ms(a, Clock::now()); });
        const double par = median(7, [&] {
            std::atomic<uint64_t> acc{0};
            const auto a = Clock::now();
            jobs::parallelFor("bench", N, 16384, [&](uint32_t b, uint32_t e) { acc.fetch_add((uint64_t)work(b, e), std::memory_order_relaxed); });
            keep = (double)acc.load();
            return ms(a, Clock::now());
        });
        std::printf("pfor.scale        %8.2f ms parallel, %.2f ms serial: %.1fx on %u threads\n", par, serial, serial / par, threads);
    }
    // ── frame.mix ───────────────────────────────────────────────────────────
    {
        const uint32_t N = 200000;
        std::vector<float> v(N, 1.0f);
        const double t = median(9, [&] {
            std::atomic<int> small{0};
            std::vector<jobs::JobHandle> hs(200);
            const auto a = Clock::now();
            for (auto& h : hs) h = jobs::run("phys", [&small] { volatile double x = 0; for (int i = 0; i < 2000; ++i) x = x + std::sqrt((double)i); small.fetch_add(1); });
            for (int pass = 0; pass < 6; ++pass)
                jobs::parallelFor("pass", N, 2048, [&](uint32_t b, uint32_t e) { for (uint32_t i = b; i < e; ++i) v[i] = std::sqrt(v[i] + 1.0f); });
            for (auto& h : hs) jobs::wait(h);
            const auto b = Clock::now();
            for (auto& h : hs) h = {};
            jobs::pumpMain();
            return ms(a, b);
        });
        std::printf("frame.mix         %8.3f ms/frame (6 parallelFors + 200 jobs)\n", t);
    }
    // ── frame.paced: the same frame, once every 4 ms, as a game runs it ─────
    // Back-to-back frames flatter a scheduler that spins. Here the pool idles
    // between frames, so this is where sleeping too eagerly (slow frames) and
    // spinning too long (CPU burned doing nothing) both show.
    {
        const uint32_t N = 200000;
        std::vector<float> v(N, 1.0f);
        std::vector<double> times;
        const std::clock_t c0 = std::clock();
        const auto w0 = Clock::now();
        for (int f = 0; f < 150; ++f) {
            std::atomic<int> small{0};
            std::vector<jobs::JobHandle> hs(200);
            const auto a = Clock::now();
            for (auto& h : hs) h = jobs::run("phys", [&small] { volatile double x = 0; for (int i = 0; i < 2000; ++i) x = x + std::sqrt((double)i); small.fetch_add(1); });
            for (int pass = 0; pass < 6; ++pass)
                jobs::parallelFor("pass", N, 2048, [&](uint32_t b, uint32_t e) { for (uint32_t i = b; i < e; ++i) v[i] = std::sqrt(v[i] + 1.0f); });
            for (auto& h : hs) jobs::wait(h);
            times.push_back(ms(a, Clock::now()));
            for (auto& h : hs) h = {};
            jobs::pumpMain();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        std::sort(times.begin(), times.end());
        const double cpu = double(std::clock() - c0) / CLOCKS_PER_SEC, wall = ms(w0, Clock::now()) / 1000.0;
        std::printf("frame.paced       %8.3f ms median, %.3f ms worst of 150; CPU %.0f%% of one core over the run\n",
                    times[times.size() / 2], times.back(), 100.0 * cpu / wall);
    }
    // ── idle: nothing submitted for half a second ──────────────────────────
    {
        const std::clock_t c0 = std::clock();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("idle              %8.2f ms of CPU in 500 ms with nothing to do\n",
                    1000.0 * double(std::clock() - c0) / CLOCKS_PER_SEC);
    }
    (void)cpu0; (void)wall0;
    const jobs::Stats st = jobs::stats();
    std::printf("backend %s: %llu jobs, %llu steals, %llu parks, %llu wakes, %llu run by the submitter\n",
                jobs::backendName(), (unsigned long long)st.jobsRun, (unsigned long long)st.steals,
                (unsigned long long)st.parks, (unsigned long long)st.wakes, (unsigned long long)st.inlineRuns);
    jobs::shutdown();
    return 0;
}
