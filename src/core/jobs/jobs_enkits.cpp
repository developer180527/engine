// ── engine::jobs — enkiTS backend ───────────────────────────────────────────
// The ONLY translation unit in the engine that includes enkiTS. Everything
// here maps the facade's counter-style contract (jobs.h) onto enki task sets;
// a FiberTaskingLib backend would be a sibling .cpp mapping the same contract
// onto ftl::TaskScheduler + WaitForCounter, selected in src/CMakeLists.txt.
#include "core/jobs/jobs.h"
#include "core/jobs/jobs_shared.h"

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

// run() job block. The in-flight registry below and any caller-held JobHandles
// each hold a reference: pumpMain()'s sweep drops the REGISTRY's once complete,
// but a stashed handle keeps the block alive, so wait() on an old handle is
// always safe (audit C.5 — the raw-pointer handle was a use-after-free once the
// sweep freed the block).
struct RunTask final : public enki::ITaskSet, detail::JobRef {
    const char*            name;
    detail::InlineCallable fn;
    explicit RunTask(const char* n) : enki::ITaskSet(1), name(n) {
        destroy = [](detail::JobRef* r) {
            RunTask* t = static_cast<RunTask*>(r);
            t->~RunTask();
            mem::free(t);
        };
    }
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        // NO profiler scope here, on purpose: run() jobs are ASYNC and may
        // span profiler frame boundaries, but TimerChannel::beginFrame
        // clears every thread's sample vectors assuming all scopes closed
        // (the "frame boundary is a sync point" invariant). An open scope
        // racing that clear corrupts the vectors — multi-second clear()
        // stalls and an unresponsive app. parallelFor keeps its scope: it
        // BLOCKS, so its scopes always close within the frame.
        {
            ScratchScope scratch;
            fn.invoke();
        }
        fn.reset();
    }
};

std::mutex            g_inflightMu;
std::vector<RunTask*> g_inflight;   // each holds the registry's reference

// Blocking parallelFor task — lives on the caller's stack, so no registry.
struct ForTask final : public enki::ITaskSet {
    const char* name;
    const std::function<void(uint32_t, uint32_t)>* fn;
    void ExecuteRange(enki::TaskSetPartition r, uint32_t) override {
        ENGINE_PROFILE_SCOPE(name);
        ScratchScope scratch;
        (*fn)(r.start, r.end);
    }
};

} // namespace

const char* backendName() { return "enkits"; }

void init(uint32_t numWorkers, int /*spinMicros: enkiTS fixes its own*/) {
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
    {
        // enkiTS makes its workers with std::thread: their bookkeeping must come
        // from the system allocator (mem::SystemNewScope). Its own arrays still
        // come from Tag::Jobs through the allocator above.
        mem::SystemNewScope systemNew;
        g_ts.Initialize(cfg);
    }
    detail::startShared();

    g_init.store(true);
    LOG_INFO("Jobs", "worker pool up: %u task threads (workers + main), spawned once",
             g_ts.GetNumTaskThreads());
}

void shutdown() {
    if (!g_init.load()) return;
    detail::shutdownShared();   // the I/O lane first, while the pool still takes work
    g_ts.WaitforAllAndShutdown();
    g_init.store(false);
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        for (RunTask* t : g_inflight) t->release();
        g_inflight.clear();
    }
    LOG_INFO("Jobs", "worker pool down");
}

bool initialized() { return g_init.load(); }

uint32_t workerCount() {
    return g_init.load() ? g_ts.GetNumTaskThreads() : 1u;
}

JobHandle detail::runErased(const char* name, void* callable, size_t size, size_t align,
                            const CallableOps& ops, Priority priority) {
    // No pool (headless tools, early boot), or an unregistered external thread
    // that would share slot 0 with the main thread: run inline. The work still
    // happens, on this thread, and a default handle reads as already-complete.
    if (!g_init.load() || !ensureThreadRegistered()) {
        ops.invoke(callable);
        ops.destroy(callable);
        return {};
    }

    RunTask* task = new (mem::alloc(sizeof(RunTask), alignof(RunTask) < 16 ? 16 : alignof(RunTask),
                                    mem::Tag::Jobs)) RunTask(name);
    task->fn.take(callable, size, align, ops);
    task->m_Priority = (enki::TaskPriority)priority;
    task->retain();   // the handle's; the registry holds the first

    // ── PIPE FIRST, THEN REGISTER, and the order is the whole fix ───────────
    // enkiTS's GetIsComplete() is `m_RunningCount == 0`, and m_RunningCount
    // starts at ZERO — so a task that has been constructed but not yet added to
    // the pipe reads as COMPLETE. Registering it in that state opened a window:
    //
    //     push to g_inflight        (reads complete: running count still 0)
    //   > pumpMain() sweeps it out  (registry ref dropped — it "finished")
    //     AddTaskSetToPipe          (running count 1; now genuinely pending)
    //     return, caller discards the handle -> refcount 0 -> ~RunTask
    //     -> ~ICompletable -> ENKI_ASSERT(GetIsComplete()) on a QUEUED task
    //
    // Every jobs::run caller that ignores the handle is exposed, which is most
    // of them (JoltJobsAdapter::QueueJob among them). It needed the sweep to
    // land inside a two-statement window, hence load-dependent (BUG-0056).
    //
    // Adding to the pipe first closes it: after this call the task either has a
    // non-zero running count or has genuinely finished, so the sweep's
    // completion test means what the registry assumes it means. The handle's
    // reference keeps the block alive across both steps, and pushing an
    // already-finished task is harmless — the next sweep removes it.
    g_ts.AddTaskSetToPipe(task);
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        g_inflight.push_back(task);
    }
    return JobHandle(task, 0);   // the handle co-owns the block — stashing is safe
}

void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t, uint32_t)>& fn, Priority priority) {
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
    t.m_Priority = (enki::TaskPriority)priority;
    g_ts.AddTaskSetToPipe(&t);
    g_ts.WaitforTask(&t);   // caller works on ranges too — never idles
}

void wait(JobHandle h) {
    if (!h.valid() || !g_init.load()) return;
    if (h.lane == 1) {   // an I/O lane job: not an enkiTS task
        detail::JobRef* r = h.ref;
        detail::helpUntil([r] { return r->done.load(std::memory_order_acquire); });
        return;
    }
    // WaitforTask makes the calling thread help run tasks, so it needs a slot
    // for the same reason. Without one, spin here rather than corrupt slot 0.
    if (!ensureThreadRegistered()) {
        auto* t = static_cast<RunTask*>(h.ref);
        while (!t->GetIsComplete()) std::this_thread::yield();
        return;
    }
    // h co-owns the RunTask: even if pumpMain() swept it from g_inflight
    // long ago, the block is alive and WaitforTask on a completed task
    // returns immediately.
    g_ts.WaitforTask(static_cast<RunTask*>(h.ref));
}

void frameBegin() {}   // enkiTS wakes its own threads

// ── What jobs_shared.cpp needs from a backend ───────────────────────────────
// enkiTS has no "run one job" call, so a thread waiting on an I/O job does not
// help the pool here; it relaxes and parks.
bool detail::tryRunOne()    { return false; }
bool detail::hasPending()   { return false; }
int  detail::spinMicros()   { return 50; }
bool detail::onMainThread() { return !g_init.load() || std::this_thread::get_id() == g_mainThread; }
void detail::backendSweep() {
    // Drop the registry's reference to finished run() tasks. Blocks whose
    // handles were discarded die here; stashed handles keep theirs alive.
    std::lock_guard<std::mutex> lk(g_inflightMu);
    for (size_t i = g_inflight.size(); i-- > 0;) {
        if (g_inflight[i]->GetIsComplete()) {
            g_inflight[i]->release();
            g_inflight[i] = g_inflight.back();
            g_inflight.pop_back();
        }
    }
}

Stats stats() { Stats s{}; s.ioJobs = detail::ioJobsRun(); return s; }

} // namespace jobs
