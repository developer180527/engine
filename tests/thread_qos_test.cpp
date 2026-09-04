// ── thread_qos_test — the scheduling classes actually land on the threads
//
// The whole of engine::qos is fire-and-forget: nothing reads back what it set,
// no frame depends on it, and a silent failure produces a build that is merely
// a bit slower on a machine we do not own. That is the exact profile of a call
// site that quietly stops running — the repo's standing rule is that a gate
// which has never executed under test is a comment, and a setter nobody
// asserts on is the same thing.
//
// The property under test is NOT "QoS makes things faster". That was measured
// once (14.5x under contention, core/thread_qos.h) and is a property of the OS,
// not of this code. What can rot HERE is narrower and entirely checkable:
//
//   * the job pool's workers get Initiated rather than the DEFAULT class
//     pthread_create hands out;
//   * a thread we classify stays classified;
//   * external threads — a kit's, a provider's — are NOT touched.
//
// The last one matters most and is the easiest to break: enkiTS's threadStart
// fires only inside TaskingThreadFunction, so registering an external thread
// must leave that thread's class alone. If a future backend swap started
// classifying registered threads too, the engine would begin silently
// reclassifying threads it does not own.
//
// PLATFORM HONESTY: only Apple can report a thread's real class back, so only
// there does this test prove the OS agrees with us. Elsewhere
// engine::qos::currentThreadClass() echoes our own last call, which cannot
// catch the OS overriding us — so the strong assertions are gated on
// classIsObservable() rather than on a platform macro, and a platform that
// later gains a real getter strengthens this test without editing it.
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "core/thread_qos.h"
#include "runtime/jobs/jobs.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using engine::qos::Class;

static const char* name(Class c) {
    switch (c) {
        case Class::Interactive:  return "Interactive";
        case Class::Initiated:    return "Initiated";
        case Class::Utility:      return "Utility";
        case Class::Unclassified: return "Unclassified";
    }
    return "?";
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("thread_qos_test: scheduling classes land where they are aimed\n");

    const bool observable = engine::qos::classIsObservable();
    std::printf("  (class readback is %s on this platform)\n",
                observable ? "OS-BACKED" : "echo-only — strong checks skipped");

    // ── 1. The setter round-trips on the calling thread ─────────────────────
    {
        std::printf("\n-- 1. set and read back --\n");
        std::atomic<bool> ok{false};
        std::atomic<int>  got{-1};
        // On a fresh thread, so this cannot pass by accident on whatever class
        // main() happens to hold.
        std::thread t([&] {
            ok.store(engine::qos::setForCurrentThread(Class::Utility));
            got.store(static_cast<int>(engine::qos::currentThreadClass()));
        });
        t.join();
        CHECK(ok.load(), "setForCurrentThread(Utility) reported success");
        if (observable)
            CHECK(static_cast<Class>(got.load()) == Class::Utility,
                  "and the thread reads back Utility (got %s)",
                  name(static_cast<Class>(got.load())));
    }

    // ── 2. A raw thread starts unclassified ────────────────────────────────
    // The defect being fixed, asserted directly: pthread_create does NOT
    // inherit the creator's class. Without this, section 3's result could be
    // explained by inheritance rather than by our hook, and the suite would
    // pass even with the threadStart callback deleted.
    //
    // It asserts Unclassified specifically, not merely "!= Interactive". The
    // first version of this test made the weaker claim and passed on a
    // fallback constant rather than on anything the OS said — the getter had
    // no case for QOS_CLASS_DEFAULT (21), so an unclassified thread reported
    // whatever the echo happened to hold. Asserting the exact value is what
    // makes this check able to fail.
    if (observable) {
        std::printf("\n-- 2. an unclassified thread does not inherit --\n");
        engine::qos::setForCurrentThread(Class::Interactive);
        std::atomic<int> got{-1};
        std::thread t([&] { got.store(static_cast<int>(engine::qos::currentThreadClass())); });
        t.join();
        CHECK(static_cast<Class>(got.load()) == Class::Unclassified,
              "a thread created by an Interactive thread reports Unclassified "
              "(got %s) — it inherits nothing, which is why the pool must be "
              "told explicitly", name(static_cast<Class>(got.load())));
    }

    // ── 2b. The read-only sentinel cannot be set ───────────────────────────
    {
        std::printf("\n-- 2b. Unclassified is read-only --\n");
        CHECK(!engine::qos::setForCurrentThread(Class::Unclassified),
              "setForCurrentThread(Unclassified) is refused");
    }

    // ── 3. Every job-pool worker is Initiated ──────────────────────────────
    // The load-bearing assertion. The facade exposes no thread index, so this
    // collects (thread id -> class) into a map from inside the job bodies
    // rather than indexing by worker number. That is also the more honest
    // shape: the property is about every thread that RAN, not about slots.
    {
        std::printf("\n-- 3. the job pool --\n");
        // main() must hold a class distinguishable from the pool's, or "the
        // caller was not demoted" below cannot fail.
        engine::qos::setForCurrentThread(Class::Interactive);
        const auto mainId = std::this_thread::get_id();

        jobs::init(0);

        std::mutex mtx;
        std::map<std::thread::id, Class> seen;

        // Far more items than threads, with real work in each range and a
        // grain of 1, so the scheduler has both reason and opportunity to
        // spread across the whole pool.
        constexpr uint32_t kItems = 4096;
        jobs::parallelFor("qos.probe", kItems, 1,
                          [&](uint32_t begin, uint32_t end) {
            const Class c = engine::qos::currentThreadClass();
            {
                std::lock_guard<std::mutex> lk(mtx);
                seen[std::this_thread::get_id()] = c;
            }
            volatile double sink = 0.0;
            for (uint32_t i = begin; i < end; ++i) sink += i * 0.5;
            (void)sink;
        });

        int workers = 0, wrong = 0;
        for (const auto& [id, c] : seen) {
            // parallelFor documents that the CALLING thread works too, so the
            // main thread legitimately appears here. It is Interactive and must
            // stay that way — classifying it as Initiated would demote the
            // frame thread, the precise inversion thread_qos.h warns about.
            if (id == mainId) continue;
            ++workers;
            if (c != Class::Initiated) ++wrong;
        }

        std::printf("        %zu distinct thread(s) ran; %d were pool workers\n",
                    seen.size(), workers);
        CHECK(workers > 0, "at least one pool worker ran a body "
              "(workerCount() = %u)", jobs::workerCount());
        if (observable) {
            CHECK(wrong == 0,
                  "every pool WORKER that ran reports Initiated (%d did not)",
                  wrong);
            CHECK(engine::qos::currentThreadClass() == Class::Interactive,
                  "and the calling thread was NOT demoted to the worker class "
                  "(it is %s)", name(engine::qos::currentThreadClass()));
        }

        jobs::shutdown();
    }

    // ── 4. External threads are left alone ─────────────────────────────────
    // kExternalThreadSlots exists so a kit or a provider can hand the engine a
    // thread it already owns; registration happens lazily inside the first jobs
    // call from that thread (ensureThreadRegistered, the BUG-0003 fix). It must
    // not reclassify: enkiTS's threadStart fires only inside
    // TaskingThreadFunction, which a registered external thread never enters.
    //
    // Asserting it means a future backend swap that broke the property fails
    // loudly instead of silently retuning somebody else's scheduler hint —
    // which for an audio provider's decode thread would be a real regression.
    if (observable) {
        std::printf("\n-- 4. an external thread keeps its own class --\n");
        jobs::init(0);
        std::atomic<int> before{-1}, after{-1};
        std::thread t([&] {
            engine::qos::setForCurrentThread(Class::Utility);
            before.store(static_cast<int>(engine::qos::currentThreadClass()));
            // Any jobs call from this thread triggers registration.
            jobs::parallelFor("qos.external", 64, 1,
                              [](uint32_t, uint32_t) {});
            after.store(static_cast<int>(engine::qos::currentThreadClass()));
        });
        t.join();
        CHECK(before.load() == after.load(),
              "registering did not change it (%s -> %s)",
              name(static_cast<Class>(before.load())),
              name(static_cast<Class>(after.load())));
        CHECK(static_cast<Class>(after.load()) == Class::Utility,
              "the thread kept the class ITS OWNER chose");
        jobs::shutdown();
    }

    if (g_failures) {
        std::printf("\nthread_qos_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nthread_qos_test: ALL PASS\n");
    return 0;
}
