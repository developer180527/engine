// ── stress_jobs — the job facade under sustained contention ─────────────────
//
// `engine::jobs` has eight dependents and, until this file, one test:
// `jobs_test`, which checks the facade's FUNCTIONAL contract — run/wait,
// stashed handles, parallelFor coverage, onMain ordering — with no contention
// and no external threads. `docs/plans/subsystem-audit.md` names this subsystem
// as the only one meeting all three of its criticality conditions: eight
// dependents, a failure mode that is silent, and no endurance lane.
//
// The silent failure is not hypothetical. BUG-0003: every thread the engine did
// not spawn shared enkiTS's thread slot 0 with the MAIN thread, because an
// unregistered thread's `GetThreadNum()` returns 0. Two threads driving one
// slot corrupt the per-thread pipe. It reproduced about one run in five under
// TSan and never without it, and the ordinary lane could not see it at all.
//
// ── Why this test is written against the FACADE and never enkiTS ────────────
// `jobs.h` exists to be swapped: its whole shape — counter-based handles, a
// facade-owned main-thread queue, waits that help — is documented as the
// contract a fiber backend (FiberTaskingLib) must satisfy by writing one .cpp.
//
// So every assertion below is a property of `jobs.h`, expressed in terms a
// fiber scheduler must also satisfy. Nothing here reads a thread number,
// counts workers against `hardware_concurrency`, or assumes a wait runs work on
// the waiting thread. **That makes this file the acceptance gate for that
// swap**: an FTL backend that passes it is behaviourally compatible, and one
// that fails names the property it broke instead of surfacing as a hang in
// animation three weeks later.
//
// ── What actually differs between the two backends, and what does not ───────
// Worth stating precisely, because it is easy to get backwards.
//
//   * WORK STEALING IS NOT THE DIFFERENCE. enkiTS already steals:
//     `TaskScheduler::TryRunTask` reads its own pipe front, then walks every
//     other thread's pipe back-to-front, and keeps a `hintPipeToCheck` so a
//     thread that stole successfully looks there first next time. Case 4 below
//     measures that distribution rather than arguing about it.
//   * THE DIFFERENCE IS WHAT A WAIT DOES. enkiTS *helps*: the waiting thread
//     runs other tasks on its own stack, so a nested wait grows the stack and
//     the continuation always resumes on the thread that blocked. FTL
//     *suspends*: the fiber yields, the thread picks up unrelated work, and the
//     continuation may resume on a DIFFERENT thread with a fresh stack.
//
// That is why case 3 (nested waits) and case 2 (external threads) are the two
// that matter most for the migration, and why neither is in `jobs_test`.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "runtime/jobs/jobs.h"
#include "test_watchdog.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main(int argc, char** argv) {
    // The watchdog, because the headline failure mode here is a LIVELOCK
    // (BUG-0003) and a livelock's natural symptom is ctest killing the test at
    // 600s with no output. `testwd` fires first and names the phase it was in,
    // which turns "stress_jobs timed out" into "stress_jobs hung in external
    // threads" — the difference between a bug report and a shrug.
    testwd::begin("stress_jobs", 90);

    // Scale knob, so the same binary is a CI smoke test and a real soak.
    // Default is a CI SMOKE size. A real soak is `./stress_jobs 5000`.
    const int rounds = (argc > 1) ? std::atoi(argv[1]) : 100;
    std::printf("stress_jobs: %d rounds\n", rounds);

    jobs::init();
    CHECK(jobs::initialized(), "pool up (%u threads)", jobs::workerCount());
    const uint32_t nThreads = jobs::workerCount();

    testwd::phase("1: sustained churn");
    // ── 1. Sustained churn: every job runs exactly once ─────────────────────
    // The base property everything else rests on. A lost job and a
    // double-executed job both show up here and nowhere else in the suite,
    // because `jobs_test` schedules a handful of jobs with nothing else running.
    {
        std::atomic<int> ran{0};
        std::vector<jobs::JobHandle> handles;
        handles.reserve(rounds * 4);
        for (int i = 0; i < rounds * 4; ++i)
            handles.push_back(jobs::run("stress.churn",
                                        [&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
        for (auto& h : handles) jobs::wait(h);
        CHECK(ran.load() == rounds * 4,
              "every scheduled job ran exactly once (%d of %d)",
              ran.load(), rounds * 4);
    }

    testwd::phase("2: external threads (BUG-0003)");
    // ── 2. External threads: BUG-0003's exact shape ─────────────────────────
    // Threads the engine did not spawn, all entering the scheduler at once.
    // Before the fix each shared slot 0 with the main thread and corrupted the
    // per-thread pipe; the symptom was a LIVELOCK, so the assertion that
    // matters is simply that this section finishes.
    //
    // This is also the case most likely to break under FTL, and for a different
    // reason: a fiber scheduler needs a thread to be fiber-ready before it can
    // run or wait on tasks, so "any std::thread may call jobs::run" is a facade
    // promise the new backend has to keep deliberately rather than inherit.
    //
    // BOUNDED BY ITERATIONS, not by wall clock. The first version of this ran
    // four threads flat out for three seconds, which is not a stronger test —
    // the interleavings that matter happen in the first milliseconds — and it
    // pointlessly heats the machine. A livelock now fails by ctest TIMEOUT
    // instead of spinning until someone notices.
    {
        std::atomic<int>  done{0};
        std::atomic<long> work{0};
        const int nExternal = 4;
        const int iters     = rounds;
        std::vector<std::thread> ext;
        for (int t = 0; t < nExternal; ++t) {
            ext.emplace_back([&] {
                for (int i = 0; i < iters; ++i) {
                    auto h = jobs::run("stress.ext", [&work] {
                        work.fetch_add(1, std::memory_order_relaxed);
                    });
                    jobs::wait(h);
                    jobs::parallelFor("stress.ext.pfor", 64, 16,
                        [&work](uint32_t b, uint32_t e) {
                            work.fetch_add(e - b, std::memory_order_relaxed);
                        });
                }
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& t : ext) t.join();
        CHECK(done.load() == nExternal,
              "%d unowned threads drove the scheduler concurrently and all "
              "returned (%ld units) — BUG-0003 was a livelock here",
              nExternal, work.load());
    }

    testwd::phase("3: nested waits");
    // ── 3. Nested waits: the property the fiber swap changes most ───────────
    // A job that waits on a job that waits on a job. Under enkiTS every wait
    // helps on the caller's own stack, so depth D means D nested frames of
    // scheduler recursion; under FTL each wait yields instead and the stack
    // does not grow. Both must produce the same ANSWER, which is what this
    // asserts — the mechanism is deliberately not asserted.
    //
    // Depth is kept modest on purpose. This is not a stack-exhaustion probe:
    // enkiTS's help-based wait makes deep nesting a real stack cost, and a test
    // that hunted for the limit would encode a number that is only true of the
    // current backend, on the current platform, at the current stack size.
    {
        constexpr int kDepth = 6;
        std::atomic<int> levels{0};
        // Recursive lambda: each level schedules the next and waits for it.
        std::function<void(int)> descend = [&](int d) {
            levels.fetch_add(1, std::memory_order_relaxed);
            if (d <= 0) return;
            jobs::wait(jobs::run("stress.nested", [&descend, d] { descend(d - 1); }));
        };
        for (int i = 0; i < rounds / 4 + 1; ++i) {
            levels.store(0);
            descend(kDepth);
            if (levels.load() != kDepth + 1) {
                CHECK(false, "nested wait chain lost a level (%d of %d) on "
                             "iteration %d", levels.load(), kDepth + 1, i);
                break;
            }
        }
        CHECK(levels.load() == kDepth + 1,
              "a %d-deep chain of jobs-waiting-on-jobs completes, every level "
              "reached", kDepth);
    }

    testwd::phase("4: work distribution");
    // ── 4. Work actually distributes across the pool ────────────────────────
    // Answers "does the scheduler spread work?" with a measurement instead of
    // an argument about the backend's queue design. enkiTS steals; a fiber
    // backend must also spread; this test does not care which mechanism gets
    // there, only that more than one thread executed ranges.
    //
    // Guarded on `nThreads > 1`, because a single-core runner is a legitimate
    // configuration and asserting parallelism there would be a flake, not a
    // finding. Loops until it sees a second thread or runs out of attempts, so
    // one unlucky scheduling decision does not fail the lane.
    if (nThreads > 1) {
        std::mutex m;
        std::set<std::thread::id> seen;
        for (int attempt = 0; attempt < 20 && seen.size() < 2; ++attempt) {
            jobs::parallelFor("stress.spread", 4096, 16,
                [&](uint32_t b, uint32_t e) {
                    // Enough work that a range is worth stealing.
                    volatile uint64_t sink = 0;
                    for (uint32_t i = b; i < e; ++i) sink += i * 2654435761u;
                    (void)sink;
                    std::lock_guard<std::mutex> lk(m);
                    seen.insert(std::this_thread::get_id());
                });
        }
        CHECK(seen.size() >= 2,
              "parallelFor ranges ran on %zu distinct threads of %u — the pool "
              "distributes work (enkiTS steals; any backend must spread)",
              seen.size(), nThreads);
    } else {
        std::printf("  skip  work distribution: single-threaded pool\n");
    }

    testwd::phase("5: coverage under contention");
    // ── 5. parallelFor coverage is exact under contention ───────────────────
    // jobs_test proves coverage on a quiet pool. The interesting version is with
    // the pool already loaded, because that is when ranges get split and stolen
    // — the path where a double-visit or a gap would actually appear.
    {
        constexpr uint32_t kN = 20000;
        std::vector<std::atomic<int>> hits(kN);
        for (auto& h : hits) h.store(0, std::memory_order_relaxed);

        // Contention from REAL BOUNDED WORK, never a spin loop. The first
        // version parked eight jobs in `while(!stop) yield()`, which does not
        // contend with the parallelFor so much as starve it: eight pool threads
        // burning cycles to accomplish nothing, and on a small pool the ranges
        // then had nowhere to be stolen to. Queued arithmetic occupies workers
        // the way real subsystems do, and every job terminates on its own.
        std::atomic<long> noiseWork{0};
        std::vector<jobs::JobHandle> noise;
        for (int i = 0; i < 16; ++i)
            noise.push_back(jobs::run("stress.noise", [&noiseWork] {
                uint64_t sink = 0;
                for (uint32_t k = 0; k < 200000; ++k) sink += k * 2654435761u;
                noiseWork.fetch_add((long)(sink & 1), std::memory_order_relaxed);
            }));

        jobs::parallelFor("stress.coverage", kN, 64,
            [&hits](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i)
                    hits[i].fetch_add(1, std::memory_order_relaxed);
            });

        for (auto& h : noise) jobs::wait(h);

        uint32_t wrong = 0;
        for (uint32_t i = 0; i < kN; ++i)
            if (hits[i].load() != 1) ++wrong;
        CHECK(wrong == 0,
              "every one of %u indices visited exactly once with the pool "
              "already saturated (%u wrong)", kN, wrong);
    }

    testwd::phase("6: main-thread channel");
    // ── 6. onMain under load, and drainMain leaves nothing behind ───────────
    // The main-thread channel is FACADE-owned precisely so a fiber backend needs
    // no pinned-task support, which makes it the piece most likely to be
    // reimplemented during the swap — and the one whose failure unloads a kit
    // with callbacks still queued into it (a jump into unmapped memory).
    {
        std::atomic<int> queued{0};
        std::atomic<int> ranOnMain{0};
        std::vector<jobs::JobHandle> hs;
        for (int i = 0; i < rounds; ++i)
            hs.push_back(jobs::run("stress.queue", [&] {
                queued.fetch_add(1, std::memory_order_relaxed);
                jobs::onMain([&ranOnMain] {
                    ranOnMain.fetch_add(1, std::memory_order_relaxed);
                });
            }));
        for (auto& h : hs) jobs::wait(h);

        // Bounded rounds, like the frame loop's own drain.
        jobs::drainMain(16);
        CHECK(ranOnMain.load() == queued.load(),
              "every callback queued from a job ran on the main thread "
              "(%d of %d)", ranOnMain.load(), queued.load());
        CHECK(jobs::pumpMain() == 0,
              "drainMain left the queue EMPTY — anything still queued when a "
              "dylib unloads is a call into unmapped memory");
    }

    jobs::shutdown();
    testwd::end();

    if (g_failures) {
        std::printf("\nstress_jobs: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nstress_jobs: all checks passed\n");
    return 0;
}
