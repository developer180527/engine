// ── engine::jobs — enkiTS backend ───────────────────────────────────────────
// The ONLY translation unit in the engine that includes enkiTS. Everything
// here maps the facade's counter-style contract (jobs.h) onto enki task sets;
// a FiberTaskingLib backend would be a sibling .cpp mapping the same contract
// onto ftl::TaskScheduler + WaitForCounter, selected in src/CMakeLists.txt.
#include "runtime/jobs/jobs.h"

#include <TaskScheduler.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/logger.h"
#include "core/memory/mem.h"
#include "core/profiler.h"
#include "core/thread_qos.h"

namespace jobs {
namespace {

enki::TaskScheduler g_ts;
std::atomic<bool>   g_init{false};
std::thread::id     g_mainThread;

// ── External threads must claim a slot before touching the scheduler ────────
// enkiTS gives every thread a thread number and indexes PER-THREAD state with
// it. From its own header:
//
//   "Will return 0 for thread which initialized the task scheduler, and all
//    other non-enkiTS threads which have not been registered"
//
// So an unregistered external thread silently shares slot 0 WITH THE MAIN
// THREAD. Two threads driving one slot corrupt the scheduler's per-thread pipe
// state, and the symptom is a livelock: workers spin in the lock-free pipe
// forever while the caller blocks in WaitforTask.
//
// This was live. jobs::parallelFor is documented as callable from a kit's own
// threads (api_primitives_test proves it), and the audio provider ABI tells
// providers to send decode, streaming and ray-tracing work to
// services->parallelFor from THEIR workers. Every one of those was an
// unregistered external thread. It reproduced about 1 run in 5 under TSan and
// is rare enough without it to have gone unnoticed — which is exactly the
// profile of a bug that ships (BUG-0003).
constexpr uint32_t kExternalThreadSlots = 8;

// Deregisters on thread exit so a transient thread returns its slot. Guarded on
// g_init because a thread outliving shutdown must not touch a dead scheduler.
struct ExternalSlot {
    bool held = false;
    ~ExternalSlot() {
        if (held && g_init.load()) g_ts.DeRegisterExternalTaskThread();
    }
};

// True when this thread may safely call the enkiTS API.
bool ensureThreadRegistered() {
    if (std::this_thread::get_id() == g_mainThread) return true;
    // Non-zero means a pool thread, or an external thread already registered —
    // both own their slot.
    if (g_ts.GetThreadNum() != 0) return true;

    thread_local ExternalSlot slot;
    if (!slot.held) slot.held = g_ts.RegisterExternalTaskThread();
    return slot.held;
}

// run() control block. Shared between the in-flight registry below and any
// caller-held JobHandles: pumpMain()'s sweep drops the REGISTRY's reference
// once complete, but a stashed handle keeps the block alive, so wait() on
// an old handle is always safe (audit C.5 — the raw-pointer handle was a
// use-after-free once the sweep freed the block).
struct RunTask final : public enki::ITaskSet {
    const char*           name;
    std::function<void()> fn;
    RunTask(const char* n, std::function<void()> f)
        : enki::ITaskSet(1), name(n), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        // NO profiler scope here, on purpose: run() jobs are ASYNC and may
        // span profiler frame boundaries, but TimerChannel::beginFrame
        // clears every thread's sample vectors assuming all scopes closed
        // (the "frame boundary is a sync point" invariant). An open scope
        // racing that clear corrupts the vectors — multi-second clear()
        // stalls and an unresponsive app. parallelFor keeps its scope: it
        // BLOCKS, so its scopes always close within the frame.
        fn();
    }
};

std::mutex                            g_inflightMu;
std::vector<std::shared_ptr<RunTask>> g_inflight;

std::mutex                         g_mainMu;
std::vector<std::function<void()>> g_mainQueue;
std::vector<std::function<void()>> g_mainScratch;   // drained copy (run unlocked)

// Blocking parallelFor task — lives on the caller's stack, so no registry.
struct ForTask final : public enki::ITaskSet {
    const char* name;
    const std::function<void(uint32_t, uint32_t)>* fn;
    void ExecuteRange(enki::TaskSetPartition r, uint32_t) override {
        ENGINE_PROFILE_SCOPE(name);
        (*fn)(r.start, r.end);
    }
};

} // namespace

void init(uint32_t numWorkers) {
    if (g_init.load()) return;
    g_mainThread = std::this_thread::get_id();

    enki::TaskSchedulerConfig cfg;
    if (numWorkers > 0) cfg.numTaskThreadsToCreate = numWorkers;
    // Slots for threads the engine does not own: kit threads, and a provider's
    // decode/streaming/propagation workers. Must be reserved HERE — enkiTS
    // sizes its per-thread arrays at Initialize and cannot grow them later.
    cfg.numExternalTaskThreads = kExternalThreadSlots;
    // Classify every worker the pool creates. threadStart runs ON the new
    // thread as its first act inside TaskingThreadFunction (verified in
    // third_party/enkiTS/src/TaskScheduler.cpp), which is what makes a
    // self-scoped QoS call correct here; SafeCallback is a plain null check
    // with no profiling-build guard, so this fires in every configuration.
    //
    // Necessary because pthread_create does NOT inherit the creator's class:
    // without this the pool runs at DEFAULT, one step below the main thread,
    // by omission. See core/thread_qos.h for the 14.5x measurement.
    //
    // Only INTERNAL workers reach this. Threads registered through
    // kExternalThreadSlots -- a kit's, a provider's -- never run
    // TaskingThreadFunction, and that is the right outcome: we do not own
    // them, and silently reclassifying somebody else's thread is exactly the
    // kind of action-at-a-distance the external-slot design exists to avoid.
    cfg.profilerCallbacks.threadStart = [](uint32_t) {
        engine::qos::setForCurrentThread(engine::qos::Class::Initiated);
    };
    cfg.customAllocator.alloc =
        [](size_t align, size_t size, void*, const char*, int) {
            return mem::alloc(size, align, mem::Tag::Jobs);
        };
    cfg.customAllocator.free =
        [](void* p, size_t, void*, const char*, int) { mem::free(p); };
    g_ts.Initialize(cfg);

    g_init.store(true);
    LOG_INFO("Jobs", "worker pool up: %u task threads (workers + main), spawned once",
             g_ts.GetNumTaskThreads());
}

void shutdown() {
    if (!g_init.load()) return;
    g_ts.WaitforAllAndShutdown();
    g_init.store(false);
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        g_inflight.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        g_mainQueue.clear();
    }
    LOG_INFO("Jobs", "worker pool down");
}

bool initialized() { return g_init.load(); }

uint32_t workerCount() {
    return g_init.load() ? g_ts.GetNumTaskThreads() : 1u;
}

JobHandle run(const char* name, std::function<void()> fn) {
    // No pool (headless tools, early boot): run inline. A default handle is
    // "already complete", so callers' wait() is still correct.
    if (!g_init.load()) { fn(); return {}; }

    // An unregistered external thread would share slot 0 with the main thread.
    // Running inline is the honest degradation: the work still happens, on this
    // thread, and a default handle reads as already-complete.
    if (!ensureThreadRegistered()) { fn(); return {}; }

    auto task = std::make_shared<RunTask>(name, std::move(fn));
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        g_inflight.push_back(task);
    }
    g_ts.AddTaskSetToPipe(task.get());
    return {task};   // handle co-owns the block — stashing is safe
}

void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t, uint32_t)>& fn) {
    if (count == 0) return;
    const uint32_t g = grain > 0 ? grain : 1;
    // Not worth a dispatch (or no pool): run the whole range inline.
    if (!g_init.load() || count <= g) { fn(0, count); return; }
    // Out of external slots: run inline rather than corrupt slot 0. Slower for
    // this caller, correct for everyone.
    if (!ensureThreadRegistered()) { fn(0, count); return; }

    ForTask t;
    t.name       = name;
    t.fn         = &fn;
    t.m_SetSize  = count;
    t.m_MinRange = g;
    g_ts.AddTaskSetToPipe(&t);
    g_ts.WaitforTask(&t);   // caller works on ranges too — never idles
}

void wait(JobHandle h) {
    if (!h.valid() || !g_init.load()) return;
    // WaitforTask makes the calling thread help run tasks, so it needs a slot
    // for the same reason. Without one, spin here rather than corrupt slot 0.
    if (!ensureThreadRegistered()) {
        auto* t = static_cast<RunTask*>(h.opaque.get());
        while (!t->GetIsComplete()) std::this_thread::yield();
        return;
    }
    // h co-owns the RunTask: even if pumpMain() swept it from g_inflight
    // long ago, the block is alive and WaitforTask on a completed task
    // returns immediately.
    g_ts.WaitforTask(static_cast<RunTask*>(h.opaque.get()));
}

void onMain(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_mainQueue.push_back(std::move(fn));
}

size_t pumpMain() {
    // Frame-loop only, main thread only (facade-level pinning — see jobs.h).
    if (std::this_thread::get_id() != g_mainThread && g_init.load()) {
        LOG_ERROR("Jobs", "pumpMain() off the main thread — ignoring");
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        g_mainScratch.swap(g_mainQueue);
    }
    const size_t ran = g_mainScratch.size();
    for (auto& fn : g_mainScratch) fn();
    g_mainScratch.clear();

    // Sweep finished run() tasks: drop the registry's reference. Blocks
    // whose handles were discarded die here; stashed handles keep theirs
    // alive (shared ownership — see jobs.h lifetime note).
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        for (size_t i = g_inflight.size(); i-- > 0;) {
            if (g_inflight[i]->GetIsComplete()) {
                g_inflight[i] = std::move(g_inflight.back());
                g_inflight.pop_back();
            }
        }
    }
    return ran;
}

void drainMain(int maxRounds) {
    // Rounds, not one pass: pumpMain() swaps the queue before running anything,
    // so a callback that queues more work leaves that work behind — and "behind"
    // means "still queued when the dylib it points into is unloaded".
    for (int i = 0; i < maxRounds; ++i)
        if (pumpMain() == 0) return;
    // DROPPED, not merely reported. The first version of this logged "dropping
    // them" and then returned with the queue intact — so the next pumpMain()
    // still ran the callback, which is the very thing draining before an unload
    // exists to prevent. Caught by tests/jobs_test.cpp, which pumps afterwards
    // and asserts nothing is left.
    std::vector<std::function<void()>> abandoned;
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        abandoned.swap(g_mainQueue);
    }
    if (!abandoned.empty())
        LOG_WARN("Jobs", "%zu main-thread callback(s) still queued after %d "
                 "drain rounds — DROPPED. Something re-queues itself every "
                 "round; that work will not run.", abandoned.size(), maxRounds);
    // Destroyed here, outside the lock: a callback's captures may run arbitrary
    // destructors, and holding the queue lock through them invites a deadlock
    // against a worker calling onMain.
    abandoned.clear();
}

} // namespace jobs
