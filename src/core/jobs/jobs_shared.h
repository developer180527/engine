#pragma once
// ── jobs_shared — what both job backends share (not a public header) ────────
// The main-thread queue, the I/O lane, per-thread scratch and the "help while
// you wait" loop are the same whichever scheduler runs the pool, so they live
// once, in jobs_shared.cpp, on top of four things a backend provides.
#include "core/jobs/jobs.h"

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#endif

namespace jobs::detail {

// What a JobHandle points at, for every job that is not backend-specific.
struct JobState {
    std::atomic<bool> done{false};
};

// A hint to the CPU that this is a spin loop. An instruction, not an OS call.
inline void relax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("isb");   // `yield` is a no-op on most ARM cores; isb costs a few cycles
#else
    std::this_thread::yield();
#endif
}

// ── Provided by the backend ─────────────────────────────────────────────────
bool tryRunOne();      // run one queued pool job on this thread, if it may and there is one
bool hasPending();     // is anything queued (a waiter should not sleep if so)
int  spinMicros();     // how long an idle thread looks for work before parking
bool onMainThread();   // is this the thread that called init()
void backendSweep();   // per-pump housekeeping (enkiTS frees finished run() blocks here)

// ── Provided by jobs_shared.cpp ─────────────────────────────────────────────
void notifyWaiters();              // something a waiter may be waiting for happened
void parkWaiter(int& backoff);     // sleep briefly; woken by notifyWaiters or a short timeout
void startShared();                // the pool is up: the I/O lane may start threads again
void shutdownShared();             // stop the I/O lane (it drains first), clear the main queue
uint64_t ioJobsRun();

// Run pool jobs until done() is true. A waiter is waiting for something that is
// about to happen, so with nothing to run it keeps looking for as long as an
// idle worker would (spinMicros) before it parks: parking early cost every
// parallelFor a wake-up at its tail.
template <class Pred>
void helpUntil(Pred&& done) {
    using clock = std::chrono::steady_clock;
    int idle = 0, backoff = 0;
    clock::time_point idleStart;
    while (!done()) {
        if (tryRunOne()) { idle = 0; backoff = 0; continue; }
        if (idle++ == 0) idleStart = clock::now();
        if ((idle & 31) != 0) { relax(); continue; }
        if (clock::now() - idleStart < std::chrono::microseconds(spinMicros())) { relax(); continue; }
        if (!done()) parkWaiter(backoff);
    }
}

}  // namespace jobs::detail
