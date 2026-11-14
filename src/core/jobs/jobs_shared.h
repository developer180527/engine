#pragma once
// ── jobs_shared — what both job backends share (not a public header) ────────
// The main-thread queue, the I/O lane, per-thread scratch and the "help while
// you wait" loop are the same whichever scheduler runs the pool, so they live
// once, in jobs_shared.cpp, on top of four things a backend provides.
#include "core/jobs/jobs.h"

#include <atomic>
#include <chrono>
#include <type_traits>
#include <thread>

#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#  include <immintrin.h>
#endif

namespace jobs::detail {

// A run() callable, held inside a job block. Up to kInline bytes (and 16-byte
// alignment) it lives in the block itself, which covers a lambda capturing a
// handful of pointers; anything bigger goes to the engine heap, one allocation.
struct InlineCallable {
    static constexpr size_t kInline = 64;
    const CallableOps* ops = nullptr;
    void*              fn  = nullptr;   // inl, or a heap block
    alignas(16) unsigned char inl[kInline];

    // Moves the callable at src here (src is left destroyed).
    void take(void* src, size_t size, size_t align, const CallableOps& o);
    void invoke() { ops->invoke(fn); }
    // Destroys the callable (on the thread that ran it, so captures die there).
    void reset();
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

// ── Provided by jobs_shared.cpp ─────────────────────────────────────────────
void notifyWaiters();              // something a waiter may be waiting for happened
// Sleep until notifyWaiters, unless done(ctx) or work is pending. No timeout:
// the waiter registers, then re-checks done under the lock, and notifyWaiters
// fences before it reads the count, so a completion is either seen here or
// notifies this thread. (It used to wake every 100 µs to 2 ms to cover the
// gap a check of hasPending alone left, which polled on a timer.)
void parkWaiter(bool (*done)(void*), void* ctx);
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
    using P = std::remove_reference_t<Pred>;
    int idle = 0;
    clock::time_point idleStart;
    while (!done()) {
        if (tryRunOne()) { idle = 0; continue; }
        if (idle++ == 0) idleStart = clock::now();
        if ((idle & 31) != 0) { relax(); continue; }
        if (clock::now() - idleStart < std::chrono::microseconds(spinMicros())) { relax(); continue; }
        parkWaiter([](void* p) -> bool { return (*static_cast<P*>(p))(); }, &done);
        idle = 0;
    }
}

}  // namespace jobs::detail
