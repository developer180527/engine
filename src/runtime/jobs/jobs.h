#pragma once
// ── engine::jobs — the engine-wide task scheduler facade ────────────────────
//
// ONE worker pool for the whole engine. Threads are spawned once at init and
// die at shutdown — thread creation is a boot event, never a frame event
// (syscall minimization). Every subsystem that wants parallelism schedules
// through this facade: animation sampling, Jolt physics steps (via the
// JPH::JobSystem adapter), async loading, cooking.
//
// BACKEND CONTRACT (why this header looks the way it does)
// The current backend is enkiTS (jobs_enkits.cpp). The API is deliberately
// shaped so the backend can be swapped for a fiber scheduler
// (FiberTaskingLib) by writing one new .cpp — no call-site changes:
//
//   * Dependencies are COUNTER-based, not task-object-based. run() returns a
//     JobHandle that behaves like FTL's AtomicCounter: wait(h) blocks until
//     every job scheduled on h has finished. enkiTS maps this onto
//     ICompletable; FTL maps it onto WaitForCounter.
//   * wait() HELPS: the waiting thread executes other jobs (enkiTS) or the
//     fiber yields and the thread picks up other work (FTL). Either way,
//     waiting never idles a core — but it also means the code AFTER wait()
//     may resume on a DIFFERENT OS thread under fibers. Hence the rules:
//       - do NOT cache thread-local state across a wait()/parallelFor() call
//       - do NOT hold a mutex across a wait()/parallelFor() call
//       - do NOT call OS TLS-dependent APIs (OpenGL contexts, etc.) around
//         waits inside jobs
//     Code that follows these rules is correct under both backends.
//   * Main-thread work is a FACADE-level queue (onMain/pumpMain), not a
//     backend pinned task — fiber schedulers have no clean pinned-task
//     notion, and bgfx/GLFW calls must land on the real main thread.
//   * Job functions are std::function; the facade preallocates nothing per
//     job today (enkiTS allocates once at init). Keep job bodies coarse —
//     this is a task scheduler, not an instruction-level parallelizer.
//   * PROFILER RULE: run() bodies must NOT open ENGINE_PROFILE_SCOPEs —
//     async jobs span profiler frame boundaries, and TimerChannel's
//     beginFrame clears per-thread buffers assuming every scope closed
//     (open scope vs clear = data race, corrupted vectors, seconds-long
//     stalls). parallelFor bodies MAY scope: it blocks, so its scopes
//     always close inside the frame.
//
// See docs/guides/engine-api.md (Jobs) — kits do NOT get this header; parallelism
// inside kits goes through the C API once that surface exists.

#include <cstdint>
#include <functional>
#include <memory>

namespace jobs {

// Opaque completion counter. Copyable; all copies refer to the same counter.
// A default-constructed handle is "already complete".
//
// LIFETIME: the handle shares ownership of the backend's completion object,
// so it is safe to stash across frames — wait() on an old handle returns
// immediately once the job completed, never dereferences freed memory.
// (Audit C.5: the previous raw-pointer handle dangled after pumpMain()'s
// sweep freed completed jobs; wait() on a stashed handle was a UAF.) A held
// handle pins a small control block, so drop handles when done with them.
struct JobHandle {
    std::shared_ptr<void> opaque;   // backend completion object (type-erased)
    bool valid() const { return opaque != nullptr; }
};

// Spawn the worker pool (hardware_concurrency - 1 workers + the calling
// thread participates in waits). Call once from EngineRuntime::init on the
// main thread. numWorkers == 0 picks the default.
void init(uint32_t numWorkers = 0);
void shutdown();
bool initialized();

// Number of threads that may execute jobs (workers + main). Sizing hint for
// per-thread scratch arrays.
uint32_t workerCount();

// ── Fire-and-track ──────────────────────────────────────────────────────────
// Schedule fn on the pool. The returned handle completes when fn returns.
// `name` feeds the profiler; use a string literal (not copied until traced).
JobHandle run(const char* name, std::function<void()> fn);

// ── Data parallelism ────────────────────────────────────────────────────────
// Split [0, count) across the pool; fn(begin, end) runs on ranges. BLOCKS
// until every range is done (the calling thread works too). grain is the
// minimum range size worth a task — pick it so one range is >= ~10µs of work.
void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t begin, uint32_t end)>& fn);

// ── Waiting ─────────────────────────────────────────────────────────────────
// Block until h completes. The calling thread executes other jobs meanwhile.
// Rules above apply: nothing thread-identity-dependent may straddle this call.
void wait(JobHandle h);

// ── Main-thread channel ─────────────────────────────────────────────────────
// Queue fn to run on the main thread during the next pumpMain(). Safe from
// any job. The frame loop calls pumpMain() once per frame; it drains the
// queue and returns. This is facade-owned so a fiber backend needs no
// pinned-task support.
void onMain(std::function<void()> fn);
void pumpMain();

} // namespace jobs
