// ── jobs_shared — the parts of engine::jobs no backend owns ─────────────────
// The main-thread queue, the I/O lane, per-thread scratch memory and waiter
// parking. Standard C++ only, like the native backend.
#include "core/jobs/jobs_shared.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "core/frame_arena.h"
#include "core/logger.h"
#include "core/memory/mem.h"
#include "core/thread_qos.h"
#include "core/thread_start.h"

namespace jobs {

// ── Waiters ─────────────────────────────────────────────────────────────────
// A thread in wait() helps first and parks last. A lost notify costs at most
// the timeout (it starts at 100 µs and backs off to 2 ms), never a hang, so
// the notify side stays a single atomic load when nobody is parked.
namespace {
std::atomic<int>        g_waiters{0};
std::mutex              g_waitMu;
std::condition_variable g_waitCv;
}

void detail::notifyWaiters() {
    if (g_waiters.load(std::memory_order_seq_cst) <= 0) return;
    { std::lock_guard<std::mutex> lk(g_waitMu); }
    g_waitCv.notify_all();
}

void detail::parkWaiter(int& backoff) {
    std::unique_lock<std::mutex> lk(g_waitMu);
    g_waiters.fetch_add(1, std::memory_order_seq_cst);
    if (!hasPending()) {
        const int us = backoff < 5 ? (100 << backoff) : 2000;
        g_waitCv.wait_for(lk, std::chrono::microseconds(us));
        if (backoff < 5) ++backoff;
    }
    g_waiters.fetch_sub(1, std::memory_order_seq_cst);
}

// ── Main-thread channel ─────────────────────────────────────────────────────
namespace {
std::mutex                         g_mainMu;
std::vector<std::function<void()>> g_mainQueue;
std::vector<std::function<void()>> g_mainScratch;   // drained copy (run unlocked)
}

void onMain(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mainMu);
    g_mainQueue.push_back(std::move(fn));
}

size_t pumpMain() {
    // Frame-loop only, main thread only.
    if (!detail::onMainThread()) {
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
    detail::backendSweep();
    return ran;
}

void drainMain(int maxRounds) {
    // Rounds, not one pass: pumpMain() swaps the queue before running anything,
    // so a callback that queues more work leaves that work behind — and "behind"
    // means "still queued when the dylib it points into is unloaded".
    for (int i = 0; i < maxRounds; ++i)
        if (pumpMain() == 0) return;
    // DROPPED, not merely reported (tests/jobs_test.cpp pumps afterwards and
    // asserts nothing is left).
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

// ── The I/O lane ────────────────────────────────────────────────────────────
// A few threads that exist to be blocked. A plain mutex and condition variable:
// these jobs take milliseconds, so the queue's cost does not matter, and its
// simplicity does.
namespace {
struct IoJob final : detail::JobState {
    std::function<void()> fn;
};
struct IoLane {
    std::mutex                          mu;
    std::condition_variable             cv;
    std::deque<std::shared_ptr<IoJob>>  queue;
    std::vector<std::thread>            threads;
    bool                                stop = false;
    bool                                closed = true;   // no pool: runIo runs inline
};
IoLane                g_io;
std::atomic<uint64_t> g_ioRun{0};

void ioMain() {
    for (;;) {
        std::shared_ptr<IoJob> job;
        {
            std::unique_lock<std::mutex> lk(g_io.mu);
            g_io.cv.wait(lk, [] { return g_io.stop || !g_io.queue.empty(); });
            if (g_io.queue.empty()) return;   // stop, and nothing left to run
            job = std::move(g_io.queue.front());
            g_io.queue.pop_front();
        }
        job->fn();
        job->fn = nullptr;
        g_ioRun.fetch_add(1, std::memory_order_relaxed);
        job->done.store(true, std::memory_order_release);
        detail::notifyWaiters();
    }
}
}  // namespace

JobHandle runIo(const char* /*name*/, std::function<void()> fn) {
    if (!initialized()) { fn(); return {}; }
    auto job = std::make_shared<IoJob>();
    job->fn = std::move(fn);
    {
        std::unique_lock<std::mutex> lk(g_io.mu);
        if (g_io.closed) {   // shutting down: the lane takes no more, the caller runs it
            lk.unlock();
            job->fn();
            return {};
        }
        if (g_io.threads.empty()) {
            // Started on first use: a game that never reads a file has no lane.
            g_io.stop = false;
            uint32_t n = std::thread::hardware_concurrency() / 4;
            if (n < 2) n = 2;
            if (n > 4) n = 4;
            for (uint32_t i = 0; i < n; ++i)
                g_io.threads.push_back(engine::threads::start(engine::qos::Class::Utility, [] { ioMain(); }));
        }
        g_io.queue.push_back(job);
    }
    g_io.cv.notify_one();
    JobHandle h;
    h.opaque = std::shared_ptr<detail::JobState>(job, static_cast<detail::JobState*>(job.get()));
    h.lane = 1;
    return h;
}

uint64_t detail::ioJobsRun() { return g_ioRun.load(std::memory_order_relaxed); }

void detail::startShared() {
    std::lock_guard<std::mutex> lk(g_io.mu);
    g_io.closed = false;
    g_io.stop = false;
}

void detail::shutdownShared() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(g_io.mu);
        g_io.stop = true;
        g_io.closed = true;
        threads.swap(g_io.threads);
    }
    g_io.cv.notify_all();
    for (std::thread& t : threads) t.join();   // each drains the queue before it leaves
    {
        std::lock_guard<std::mutex> lk(g_mainMu);
        g_mainQueue.clear();
    }
}

// ── Scratch ─────────────────────────────────────────────────────────────────
namespace {
constexpr size_t kScratchBytes = 256 * 1024;
thread_local mem::FrameArena t_scratch;
}

void* scratchAlloc(size_t bytes, size_t align) {
    if (!t_scratch.initialised()) t_scratch.init(kScratchBytes, mem::Tag::Jobs);
    return t_scratch.alloc(bytes, align);
}

ScratchScope::ScratchScope() {
    const mem::FrameArena::Mark m = t_scratch.mark();
    m_offset = m.offset;
    m_overflow = m.overflowCount;
}
ScratchScope::~ScratchScope() {
    if (t_scratch.initialised()) t_scratch.rewind({m_offset, m_overflow});
}

}  // namespace jobs
