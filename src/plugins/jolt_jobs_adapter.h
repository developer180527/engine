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

#include <chrono>
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
        jobs::run("physics.job", [job] {
            job->Execute();
            job->Release();
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
};
