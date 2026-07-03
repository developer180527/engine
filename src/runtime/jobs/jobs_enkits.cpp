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

namespace jobs {
namespace {

enki::TaskScheduler g_ts;
std::atomic<bool>   g_init{false};
std::thread::id     g_mainThread;

// run() control block. Owned by the in-flight registry below; swept by
// pumpMain() once complete. This is why handles must not be stashed across
// frames (documented in jobs.h) — the block is gone after the sweep.
struct RunTask final : public enki::ITaskSet {
    const char*           name;
    std::function<void()> fn;
    RunTask(const char* n, std::function<void()> f)
        : enki::ITaskSet(1), name(n), fn(std::move(f)) {}
    void ExecuteRange(enki::TaskSetPartition, uint32_t) override {
        ENGINE_PROFILE_SCOPE(name);
        fn();
    }
};

std::mutex                            g_inflightMu;
std::vector<std::unique_ptr<RunTask>> g_inflight;

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

    auto task = std::make_unique<RunTask>(name, std::move(fn));
    RunTask* raw = task.get();
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        g_inflight.push_back(std::move(task));
    }
    g_ts.AddTaskSetToPipe(raw);
    return {raw};
}

void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t, uint32_t)>& fn) {
    if (count == 0) return;
    const uint32_t g = grain > 0 ? grain : 1;
    // Not worth a dispatch (or no pool): run the whole range inline.
    if (!g_init.load() || count <= g) { fn(0, count); return; }

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
    g_ts.WaitforTask(static_cast<RunTask*>(h.opaque));
}

void onMain(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_mainQueue.push_back(std::move(fn));
}

void pumpMain() {
    // Frame-loop only, main thread only (facade-level pinning — see jobs.h).
    if (std::this_thread::get_id() != g_mainThread && g_init.load()) {
        LOG_ERROR("Jobs", "pumpMain() off the main thread — ignoring");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        g_mainScratch.swap(g_mainQueue);
    }
    for (auto& fn : g_mainScratch) fn();
    g_mainScratch.clear();

    // Sweep finished run() tasks. Completed blocks die here — the once-per-
    // frame cadence is the handle-lifetime rule in jobs.h.
    {
        std::lock_guard<std::mutex> lk(g_inflightMu);
        for (size_t i = g_inflight.size(); i-- > 0;) {
            if (g_inflight[i]->GetIsComplete()) {
                g_inflight[i] = std::move(g_inflight.back());
                g_inflight.pop_back();
            }
        }
    }
}

} // namespace jobs
