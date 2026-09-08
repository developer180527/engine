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
    ~JoltJobsAdapter() override {
        // DROP anything still deferred, without running it. Those jobs each
        // hold a reference taken in QueueJob, and a Job whose refcount never
        // reaches zero is never returned to the free list — which is exactly
        // what the assert below checks. Executing them here instead would run
        // physics work against a PhysicsSystem that is being torn down; the
        // simulation is over by this point (onSimulationStop runs after
        // Update has returned and its barriers have completed), so releasing
        // is both safe and correct.
        {
            std::vector<Job*> pending;
            { std::lock_guard lock(m_deferMtx); pending.swap(m_deferred); }
            for (Job* j : pending) j->Release();
        }
        // 30s, and the number is not a latency budget. This deadline exists to
        // turn a use-after-free into a LOUD failure rather than a silent one,
        // so it should only ever fire on a genuine stall. It was 5s and that
        // was too tight: measured, it aborted once in twenty runs with four
        // competing physics processes saturating the machine, where the pool is
        // legitimately starved rather than stuck.
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(30);
        while (m_queued.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                // Report rather than hang or corrupt. If this ever fires, the
                // pool stopped draining and the next line is a real UAF.
                std::fprintf(stderr, "[Jolt] FATAL: %u physics job(s) still "
                             "queued after 30s; refusing to free the job list "
                             "under them.\n",
                             m_queued.load(std::memory_order_relaxed));
                std::abort();
            }
            std::this_thread::yield();
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
    void flushDeferred() {
        std::vector<Job*> pending;
        { std::lock_guard lock(m_deferMtx); pending.swap(m_deferred); }
        for (Job* j : pending) { submit(j); j->Release(); }
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
