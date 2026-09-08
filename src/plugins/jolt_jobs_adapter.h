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
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(5);
        while (m_queued.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                // Report rather than hang or corrupt. If this ever fires, the
                // pool stopped draining and the next line is a real UAF.
                std::fprintf(stderr, "[Jolt] FATAL: %u physics job(s) still "
                             "queued after 5s; refusing to free the job list "
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

    JobHandle CreateJob(const char* name, JPH::ColorArg color,
                        const JobFunction& fn,
                        JPH::uint32 numDependencies = 0) override {
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
        // Ref held for the queue (released after execution), exactly like
        // JobSystemThreadPool. Execute() no-ops if the barrier wait already
        // ran this job.
        job->AddRef();
        m_queued.fetch_add(1, std::memory_order_relaxed);
        jobs::run("physics.job", [this, job] {
            job->Execute();
            job->Release();
            // RELEASED LAST, and after Release: the destructor treats zero as
            // "no lambda can still touch m_jobs", so decrementing any earlier
            // would reopen the window it exists to close.
            m_queued.fetch_sub(1, std::memory_order_release);
        });
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
};
