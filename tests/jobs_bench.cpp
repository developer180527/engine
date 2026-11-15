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
//   allocs            global-new allocations made per run() job, 20000 held at once
//   allocs.steady     the same over frames of 200 jobs whose handles are dropped
//   frame.paced       frame.mix once every 4 ms, with the CPU the whole process used
//   frame.prewake.*   the same after 50 µs of main-thread work, without and with frameBegin()
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
#include <cstring>
#include <string>
#include <ctime>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#include <thread>
#include <vector>

// ── Results, for machines ───────────────────────────────────────────────────
// --json <file> writes every measurement with its unit and the run's identity
// (backend, OS, CPU architecture, compiler, thread count). CI uploads these and
// tests/corebench/summarise.py turns a set of them into a table; the home server's
// results database reads the same file (docs/plans/farm-and-perf-ci.md).
// CPU time this process has used, in seconds: user + system, every thread.
// NOT std::clock() on Windows: MSVC's clock() is elapsed WALL time, so the
// "CPU while idle" row read 500 ms per 500 ms there (a full core, apparently)
// on every Windows leg of perf.yml until 2026-11-15.
static double cpuSeconds() {
#if defined(_WIN32)
    FILETIME create, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return 0.0;
    auto secs = [](const FILETIME& f) {
        return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;   // 100 ns units
    };
    return secs(kernel) + secs(user);
#else
    return double(std::clock()) / CLOCKS_PER_SEC;
#endif
}

struct Result { std::string key; double value; const char* unit; };
static std::vector<Result> g_results;
static void rec(const char* key, double value, const char* unit) { g_results.push_back({key, value, unit}); }

static const char* osName() {
#if defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}
static const char* archName() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#else
    return "unknown";
#endif
}
static std::string compilerName() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}
static void writeJson(const char* path, int spin, uint32_t threads) {
    FILE* f = std::fopen(path, "w");
    if (!f) { std::fprintf(stderr, "jobs_bench: cannot write %s\n", path); return; }
    std::string comp = compilerName();
    for (char& c : comp) if (c == '"' || c == '\\') c = '\'';
    std::fprintf(f, "{\n  \"schema\": 1,\n  \"bench\": \"jobs_bench\",\n  \"backend\": \"%s\",\n"
                    "  \"os\": \"%s\",\n  \"arch\": \"%s\",\n  \"compiler\": \"%s\",\n"
                    "  \"threads\": %u,\n  \"hardware_threads\": %u,\n  \"spin_us\": %d,\n  \"results\": {\n",
                 jobs::backendName(), osName(), archName(), comp.c_str(), threads,
                 std::thread::hardware_concurrency(), spin);
    for (size_t i = 0; i < g_results.size(); ++i)
        std::fprintf(f, "    \"%s\": {\"value\": %.6g, \"unit\": \"%s\"}%s\n", g_results[i].key.c_str(),
                     g_results[i].value, g_results[i].unit, i + 1 < g_results.size() ? "," : "");
    std::fprintf(f, "  }\n}\n");
    std::fclose(f);
}

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
static bool g_quick = false;
template <class F>
static double median(int repeats, F&& f) {
    if (g_quick && repeats > 3) repeats = 3;
    std::vector<double> t;
    for (int i = 0; i < repeats; ++i) t.push_back(f());
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

int main(int argc, char** argv) {
    // jobs_bench [spinMicros] [--json <file>] [--quick]
    //   spinMicros  how long an idle worker looks before it sleeps (default: the backend's)
    //   --quick     fewer repeats, for slow machines and emulators
    int spin = -1;
    const char* jsonPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
        else if (!std::strcmp(argv[i], "--quick")) g_quick = true;
        else spin = std::atoi(argv[i]);
    }
    const double cpu0 = cpuSeconds();
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
        rec("run.dispatch", t * 1e6 / N, "ns/job");
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
        std::printf("allocs            %8.2f per run() job   (a burst: %d handles held at once)\n", double(after - before) / N, N);
        rec("allocs.per_run", double(after - before) / N, "count");
    }
    // ── allocs per run, steady state ────────────────────────────────────────
    // What a frame does: a couple of hundred jobs, waited on, handles dropped.
    // After warm-up the job blocks come back from the per-thread caches, so
    // this should read 0 on the native backend.
    {
        const int kJobs = 200, kWarm = 20, kFrames = 200;
        std::vector<jobs::JobHandle> hs(kJobs);
        std::atomic<int> ran{0};
        uint64_t before = 0;
        for (int f = 0; f < kWarm + kFrames; ++f) {
            if (f == kWarm) before = mem::stats(mem::Tag::Core).allocCount + mem::stats(mem::Tag::Jobs).allocCount;
            for (int i = 0; i < kJobs; ++i) hs[i] = jobs::run("bench", [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
            for (int i = 0; i < kJobs; ++i) jobs::wait(hs[i]);
            for (auto& h : hs) h = {};
            jobs::pumpMain();
        }
        const uint64_t after = mem::stats(mem::Tag::Core).allocCount + mem::stats(mem::Tag::Jobs).allocCount;
        const double per = double(after - before) / (double(kJobs) * kFrames);
        std::printf("allocs.steady     %8.3f per run() job   (%d frames of %d jobs, handles dropped)\n", per, kFrames, kJobs);
        rec("allocs.steady", per, "count");
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
        rec("run.dispatch.mt", t * 1e6 / (T * N), "ns/job");
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
        rec("pfor.small", par * 1e3 / N, "us/call"); rec("pfor.small.speedup", serial / par, "x");
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
        rec("pfor.scale", par, "ms"); rec("pfor.scale.speedup", serial / par, "x");
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
        rec("frame.mix", t, "ms");
    }
    // ── frame.paced: the same frame, once every 4 ms, as a game runs it ─────
    // Back-to-back frames flatter a scheduler that spins. Here the pool idles
    // between frames, so this is where sleeping too eagerly (slow frames) and
    // spinning too long (CPU burned doing nothing) both show.
    //
    // frame.prewake.off / .on: the same, but each frame starts with 50 µs of
    // main-thread work before its first job (input, scripts), with and without
    // jobs::frameBegin() at the top. The time is the jobs part only, so the
    // difference is the wake-up the first job no longer waits for.
    auto paced = [&](const char* key, bool prewake, int preworkMicros) {
        const uint32_t N = 200000;
        std::vector<float> v(N, 1.0f);
        std::vector<double> times, firstWorker;
        int alone = 0;   // frames no worker took part in: a lost wake-up looks like this
        const jobs::Stats st0 = jobs::stats();
        const double c0 = cpuSeconds();
        const auto w0 = Clock::now();
        const int frames = g_quick ? 40 : 150;
        const std::thread::id mainId = std::this_thread::get_id();
        for (int f = 0; f < frames; ++f) {
            std::atomic<int> small{0};
            // When a worker (not this thread) first starts one of this frame's
            // jobs: the wake-up the frame waited for, measured directly.
            std::atomic<int64_t> firstStart{0};
            auto mark = [&firstStart, mainId] {
                if (std::this_thread::get_id() == mainId || firstStart.load(std::memory_order_relaxed) != 0) return;
                int64_t zero = 0;
                firstStart.compare_exchange_strong(zero, Clock::now().time_since_epoch().count());
            };
            std::vector<jobs::JobHandle> hs(200);
            if (prewake) jobs::frameBegin();
            if (preworkMicros > 0) {
                const auto until = Clock::now() + std::chrono::microseconds(preworkMicros);
                while (Clock::now() < until) {}
            }
            const auto a = Clock::now();
            for (auto& h : hs) h = jobs::run("phys", [&small, &mark] { mark(); volatile double x = 0; for (int i = 0; i < 2000; ++i) x = x + std::sqrt((double)i); small.fetch_add(1); });
            for (int pass = 0; pass < 6; ++pass)
                jobs::parallelFor("pass", N, 2048, [&](uint32_t b, uint32_t e) { mark(); for (uint32_t i = b; i < e; ++i) v[i] = std::sqrt(v[i] + 1.0f); });
            for (auto& h : hs) jobs::wait(h);
            times.push_back(ms(a, Clock::now()));
            const int64_t t0 = firstStart.load();
            if (!t0) ++alone;
            if (const int64_t t = t0) firstWorker.push_back((double)(t - a.time_since_epoch().count()) * 1e-3 * Clock::period::num * 1e9 / Clock::period::den);
            for (auto& h : hs) h = {};
            jobs::pumpMain();
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        std::sort(times.begin(), times.end());
        std::sort(firstWorker.begin(), firstWorker.end());
        const double fw = firstWorker.empty() ? 0.0 : firstWorker[firstWorker.size() / 2];
        const double cpu = (cpuSeconds() - c0), wall = ms(w0, Clock::now()) / 1000.0;
        // How much of that CPU was workers awake looking for work and finding
        // none: the share a spin policy can save .
        const double spin = double(jobs::stats().spinNanos - st0.spinNanos) * 1e-9;
        std::printf("%-17s %8.3f ms median, %.3f ms worst of %d; CPU %.0f%% of one core, %.0f%% spinning (%.2f ms a frame)\n",
                    key, times[times.size() / 2], times.back(), frames, 100.0 * cpu / wall,
                    100.0 * spin / wall, 1000.0 * spin / frames);
        std::printf("%-17s %8.1f us median from the first job queued to a worker starting one; %d frames with no worker\n", "", fw, alone);
        const std::string k = key;
        rec((k + ".first_worker").c_str(), fw, "us");
        rec((k + ".alone").c_str(), alone, "frames");
        rec((k + ".spin").c_str(), 100.0 * spin / wall, "% of a core");
        rec((k + ".median").c_str(), times[times.size() / 2], "ms");
        rec((k + ".worst").c_str(), times.back(), "ms");
        rec((k + ".cpu").c_str(), 100.0 * cpu / wall, "% of a core");
    };
    paced("frame.paced", false, 0);
    paced("frame.prewake.off", false, 50);
    paced("frame.prewake.on", true, 50);
    // ── idle: nothing submitted for half a second ──────────────────────────
    {
        const double c0 = cpuSeconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("idle              %8.2f ms of CPU in 500 ms with nothing to do\n",
                    1000.0 * (cpuSeconds() - c0));
        rec("idle.cpu", 1000.0 * (cpuSeconds() - c0), "ms per 500 ms");
    }
    (void)cpu0; (void)wall0;
    const jobs::Stats st = jobs::stats();
    std::printf("backend %s: %llu jobs, %llu steals, %llu parks, %llu wakes, %llu run by the submitter\n",
                jobs::backendName(), (unsigned long long)st.jobsRun, (unsigned long long)st.steals,
                (unsigned long long)st.parks, (unsigned long long)st.wakes, (unsigned long long)st.inlineRuns);
    if (jsonPath) writeJson(jsonPath, spin, threads);
    jobs::shutdown();
    return 0;
}
