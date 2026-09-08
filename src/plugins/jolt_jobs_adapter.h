#pragma once
// ── JoltJobsAdapter — Jolt physics on the engine worker pool ────────────────
// JPH::JobSystem implementation that schedules through engine::jobs instead
// of spawning Jolt's own thread pool. ONE pool for the whole engine: physics
// no longer oversubscribes cores against animation/loading, and thread count
// stays constant from boot (syscall minimization).
//
// Mirrors JPH::JobSystemThreadPool minus the threads: JobSystemWithBarrier
// supplies dependency + barrier logic (PhysicsSystem::Update's barrier wait
// also EXECUTES queued jobs on the waiting thread, so the sim thread helps);
// we supply job storage (fixed free list, allocated once) and queueing.
// Job::Execute is internally guarded by an atomic state transition, so a job
// being picked up by both a worker and the barrier wait is safe — the loser
// of the CAS skips.

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include "runtime/jobs/jobs.h"

class JoltJobsAdapter final : public JPH::JobSystemWithBarrier {
public:
    JoltJobsAdapter(JPH::uint maxJobs, JPH::uint maxBarriers) {
        JobSystemWithBarrier::Init(maxBarriers);
        m_jobs.Init(maxJobs, maxJobs);
    }

    // ── WAIT FOR EVERY QUEUED JOB BEFORE THE FREE LIST DIES ─────────────────
    // QueueJob is fire-and-forget: it hands a raw Job* to the engine pool and
    // does not wait. Nothing else did either, so destroying this adapter while
    // a `physics.job` was still queued freed the pages that job's lambda was
    // about to touch — `job->Execute(); job->Release();` against returned
    // memory. Jolt's own ~FixedSizeFreeList assert caught it in Debug
    //
    //     JPH_ASSERT(mNumFreeObjects == mNumPages * mPageSize)
    //
    // reached from JoltPlugin::onSimulationStop. With asserts compiled out it
    // is a silent use-after-free instead. It needed CPU contention to show:
    // normally the queue has drained by the time Play stops (BUG-0055).
    //
    // Spinning is the right shape here rather than a condition variable: this
    // runs once per Play->Stop, the outstanding work is microseconds of already
    // running physics jobs, and the engine pool is still alive at this point
    // (jobs::shutdown happens later, in EngineRuntime::shutdown). Yielding lets
    // the workers finish.
    //
    // RETURNS TRUE IF THE ADAPTER IS SAFE TO DESTROY, and the caller must act
    // on false by LEAKING it. The owner has to make that call because the
    // destructor cannot: once ~JoltJobsAdapter is running there is no way to
    // un-destroy the object, so aborting is all that is left. This is a plugin,
    // and onSimulationStop runs from the editor's Stop button — killing the
    // process there costs the user their unsaved scene, while leaking one job
    // list costs a few KB. JoltPlugin::onSimulationStop does it properly.
    //
    // 30s, and the number is not a latency budget. This deadline exists to turn
    // a use-after-free into a LOUD, RECOVERABLE failure, so it should only ever
    // fire on a genuine stall. It was 5s and that was too tight: measured, it
    // fired once in twenty runs with four competing physics processes
    // saturating the machine, where the pool is legitimately starved.
    [[nodiscard]] bool drain() {
        // DROP anything still deferred, without running it. Those jobs each
        // hold a reference taken in QueueJob, and a Job whose refcount never
        // reaches zero is never returned to the free list — which is exactly
        // what Jolt's ~FixedSizeFreeList assert checks. Executing them here
        // instead would run physics work against a PhysicsSystem that is being
        // torn down; the simulation is over by this point (onSimulationStop
        // runs after Update has returned and its barriers have completed), so
        // releasing is both safe and correct.
        {
            std::vector<Job*> pending;
            { std::lock_guard lock(m_deferMtx); pending.swap(m_deferred); }
            for (Job* j : pending) j->Release();
        }
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(30);
        while (m_queued.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    ~JoltJobsAdapter() override {
        // LAST RESORT ONLY. Reaching a failed drain() here means the free list
        // is about to be released while a worker is still inside
        // `job->Execute()` against it, and there is no way to un-destroy an
        // object mid-destructor — so aborting is the honest end of the road,
        // not a policy choice. The policy choice lives at the call site, which
        // leaks this object instead. That matters because this is a PLUGIN:
        // onSimulationStop runs from the editor's Stop button, and killing the
        // process there would take the user's unsaved scene with it. A one-off
        // leak of a job list is the cheaper failure by a wide margin.
        if (!drain()) {
            std::fprintf(stderr, "[Jolt] FATAL: %u physics job(s) still queued "
                         "after 30s, and the adapter is being destroyed anyway "
                         "— the owner did not call drain(). Aborting rather "
                         "than freeing the job list under a running job.\n",
                         m_queued.load(std::memory_order_relaxed));
            std::abort();
        }
    }

    int GetMaxConcurrency() const override {
        return (int)jobs::workerCount();
    }

    // ── Am I already inside a Jolt job on THIS thread? ──────────────────────
    // The counter is bumped by the wrapper CreateJob puts around every job
    // function, so it is raised no matter WHO executes the job: our pool
    // lambda, or Jolt's own Barrier::Wait, which calls Job::Execute() directly
    // on the waiting thread. Wrapping the function rather than our lambda is
    // what makes both entry points covered by one mechanism.
    static int& depth() { static thread_local int d = 0; return d; }
    struct DepthScope {
        DepthScope()  { ++depth(); }
        ~DepthScope() { --depth(); }
    };

    JobHandle CreateJob(const char* name, JPH::ColorArg color,
                        const JobFunction& fnIn,
                        JPH::uint32 numDependencies = 0) override {
        // See QueueJob for why this wrapper exists.
        //
        // ── IT COSTS AN ALLOCATION PER JOB, AND HERE IS THE NUMBER ─────────
        // JobFunction is std::function<void()>, 32 bytes — over libc++'s
        // 24-byte inline buffer — so capturing one by value heap-allocates,
        // and Job's constructor then copies again (it takes const&, so there
        // is no move to hand it). Three constructions where the unwrapped path
        // had two.
        //
        // MEASURED 2026-09-08, because the note on JoltPlugin::m_entityToBody
        // asks for a measurement in the other direction and this deserves the
        // same bar:
        //   * 68 ns per job, wrapped vs plain, at -O2.
        //   * 13 758 CreateJob calls over 240 sim ticks in the gate's physics
        //     tier = ~57 jobs/tick, so ~3.9 us/tick, 0.23 ms per wall second.
        //   * The same tier's physics step costs ~1.6 ms/tick in this Debug
        //     build. That makes the wrapper ~0.24% of it, and the honest
        //     caveat is that Debug flatters the ratio: the wrapper number is
        //     -O2 and the step number is not. Even assuming Release makes
        //     physics 10x faster, this stays around 2%.
        //
        // KEPT at that price. The cheap alternative — raise the depth inside
        // submit()'s lambda, which is free — covers only the jobs OUR pool
        // runs. Jolt's Barrier::Wait executes jobs directly on the waiting
        // thread, and JobFindCollisions spawning from inside itself on that
        // thread is exactly the case BUG-0055 was, so dropping that coverage
        // trades a real correctness hazard for sub-1%.
        const JobFunction fn = [fnIn] { DepthScope inJob; fnIn(); };
        // Same policy as JobSystemThreadPool: the free list is sized so
        // exhaustion is a bug; if it happens, wait for jobs to complete.
        JPH::uint32 index;
        for (;;) {
            index = m_jobs.ConstructObject(name, color, this, fn, numDependencies);
            if (index != AvailableJobs::cInvalidObjectIndex) break;
            JPH_ASSERT(false, "No Jolt jobs available!");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        Job* job = &m_jobs.Get(index);
        JobHandle handle(job);   // takes a reference
        if (numDependencies == 0) QueueJob(job);
        return handle;
    }

protected:
    void QueueJob(Job* job) override {
        // ── jobs::run IS NOT ALWAYS ASYNCHRONOUS, and that is the bug ────────
        // enkiTS's SplitAndAddTask runs a task INLINE on the submitting thread
        // when that thread's pipe is full (TaskScheduler.cpp: the
        // WriterTryWriteFront failure path). Jolt spawns collision jobs from
        // INSIDE a collision job — TrySpawnJobFindCollisions, called from
        // JobFindCollisions — so under contention the inline path started a
        // Jolt job on a thread already running one.
        //
        // That is not merely untidy. JPH::BodyAccess tracks read/write
        // permission in THREAD-LOCAL state and asserts the thread is in the
        // default state when a Grant is constructed, so the nested job tripped
        // `JPH_ASSERT(velocity == EAccess::ReadWrite)`. With asserts compiled
        // out the assert is gone and the guarantee it was checking — "this
        // thread promised not to write bodies" — is silently broken instead.
        // Backtrace and analysis in docs/process/bugs/BUG-0055.
        //
        // So when we are already inside a Jolt job, DEFER rather than submit.
        // The job is not lost: PhysicsSystem adds every job it creates to the
        // step barrier immediately after CreateJob returns, and Barrier::Wait
        // executes any job it tracks — Jolt's own comment at the call site says
        // "so the main updating thread can execute the job too". Job::Execute
        // is CAS-guarded, so a job run by both the barrier and a worker is
        // safe; the loser skips.
        if (depth() > 0) {
            job->AddRef();
            std::lock_guard lock(m_deferMtx);
            m_deferred.push_back(job);
            return;
        }
        flushDeferred();
        submit(job);
    }

    // Hand a job to the pool. Only ever called with depth() == 0, so enkiTS's
    // inline path can run it on this thread without nesting.
    void submit(Job* job) {
        // Ref held for the queue (released after execution), exactly like
        // JobSystemThreadPool. Execute() no-ops if the barrier wait already
        // ran this job.
        job->AddRef();
        m_queued.fetch_add(1, std::memory_order_relaxed);
        jobs::run("physics.job", [this, job] {
            job->Execute();
            job->Release();
            // depth() is back to 0 here — the scope lives inside the job
            // FUNCTION, so it has already unwound — which makes this the first
            // safe moment to hand any jobs deferred during it to the pool.
            if (depth() == 0) flushDeferred();
            // RELEASED LAST, and after Release: the destructor treats zero as
            // "no lambda can still touch m_jobs", so decrementing any earlier
            // would reopen the window it exists to close.
            m_queued.fetch_sub(1, std::memory_order_release);
        });
    }

    // Move anything deferred while nested onto the pool. Called from the
    // non-nested QueueJob path and after every job this adapter runs, so a
    // deferred job reaches a worker as soon as any thread leaves Jolt — with
    // the step barrier as the backstop if none does.
    //
    // ── ITERATIVE, AND THE RE-ENTRY GUARD IS WHY ────────────────────────────
    // submit() -> jobs::run can run the job INLINE on this thread — the very
    // enkiTS behaviour that made QueueJob defer in the first place — and that
    // inline job ends by calling flushDeferred() again. Written as a plain
    // loop, this therefore recursed into itself once per still-deferred job,
    // with a depth bounded only by the free list (thousands) and reached
    // exactly under the contention that produces deferrals.
    //
    // The guard makes a nested call a no-op and the OUTER loop re-drains, so
    // the work is identical and the stack is one frame deep. Thread-local, not
    // a member: another worker flushing concurrently is fine and must not be
    // blocked. No lock is held across submit(), so this never deadlocks.
    // PER THREAD, and `static` so it is shared by every adapter on this thread.
    // One adapter exists (JoltPlugin owns the only one), and if a second ever
    // did, a nested flush of B inside A's flush would be skipped while only A's
    // loop re-drains — B's jobs would then wait for the step barrier. Correct,
    // but slower, and worth knowing before adding a second adapter.
    static bool& flushing() { static thread_local bool f = false; return f; }

    // RAII, NOT `busy = false` at the end, and the difference is not stylistic.
    // submit() -> jobs::run heap-allocates (make_shared<RunTask>, plus the
    // std::function), exceptions are on in this build, and nothing here
    // catches. A single bad_alloc escaping the loop would leave the flag SET
    // for the life of the thread, turning every later flushDeferred() on it
    // into a silent no-op — deferred jobs would fall back to the step barrier
    // forever. A transient failure becoming permanent silent degradation is a
    // worse bug than the recursion this guard was added to prevent.
    struct FlushGuard {
        bool& f;
        explicit FlushGuard(bool& b) : f(b) { f = true; }
        ~FlushGuard() { f = false; }
    };

    void flushDeferred() {
        bool& busy = flushing();
        if (busy) return;                  // the outer loop will take these
        FlushGuard guard(busy);
        for (;;) {
            std::vector<Job*> pending;
            { std::lock_guard lock(m_deferMtx); pending.swap(m_deferred); }
            if (pending.empty()) break;
            for (Job* j : pending) { submit(j); j->Release(); }
        }
    }

    void QueueJobs(Job** jobs, JPH::uint numJobs) override {
        for (JPH::uint i = 0; i < numJobs; ++i) QueueJob(jobs[i]);
    }

    void FreeJob(Job* job) override {
        m_jobs.DestructObject(job);
    }

private:
    using AvailableJobs = JPH::FixedSizeFreeList<Job>;
    AvailableJobs m_jobs;
    // Jobs handed to the engine pool that have not finished. The destructor's
    // wait condition; see it for why this exists.
    std::atomic<JPH::uint32> m_queued{0};

    // Jobs queued while this thread was already inside a Jolt job. See
    // QueueJob. Guarded by a plain mutex: nesting is the contended-pipe case,
    // which is rare, and the list is drained immediately.
    std::mutex        m_deferMtx;
    std::vector<Job*> m_deferred;
};
