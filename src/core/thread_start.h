#pragma once
// ── engine::threads::start — the one way the engine makes a thread ───────────
//
// Every thread the engine owns starts here (the job pool's workers through
// jobs::init, which holds the same scope around the scheduler's start-up).
// Two reasons it is one place:
//
//   * Memory. std::thread makes a small bookkeeping object with `new` that the
//     system's C++ library deletes at thread exit. It must come from the system
//     allocator, or a shared-library build of the engine aborts at the first
//     thread exit (mem::SystemNewScope, core/memory/mem.h; eng_shared_test).
//   * It is where a thread is given its scheduling class (core/thread_qos.h).
//     The overload without one keeps the OS default, and is there for threads
//     that classify themselves or have not been classified yet.
//
// engine_audit rule THREAD-01: `std::thread(` appears nowhere else in src/.
#include "core/memory/mem.h"
#include "core/thread_qos.h"

#include <thread>
#include <utility>

namespace engine::threads {

template <class Fn>
std::thread start(engine::qos::Class cls, Fn&& fn) {
    mem::SystemNewScope systemNew;   // covers std::thread's own allocations only
    return std::thread([cls, f = std::forward<Fn>(fn)]() mutable {
        engine::qos::setForCurrentThread(cls);
        f();
    });
}

template <class Fn>
std::thread start(Fn&& fn) {
    mem::SystemNewScope systemNew;
    return std::thread(std::forward<Fn>(fn));
}

}  // namespace engine::threads
