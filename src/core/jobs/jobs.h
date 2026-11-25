#pragma once
// ── engine::jobs — the engine-wide task scheduler ───────────────────────────
//
// ONE worker pool for the whole engine. Threads are spawned once at init and
// die at shutdown: thread creation is a boot event, never a frame event. Every
// subsystem that wants parallelism schedules through this header: animation,
// Jolt (via the JPH::JobSystem adapter), extraction and culling, loading.
//
// ONE BACKEND, jobs_native.cpp: work-stealing, written for this engine,
// standard C++ only (no OS call), so the platform layer can supply threads and
// parking later without touching it. Wakes only as many workers as there is new
// work, and workers spin briefly before they sleep (src/core/jobs/info.md has
// the measurements). enkiTS backed this header until 2026-11-14 (WO-079).
// tests/stress_jobs.cpp and tests/jobs_fuzz_test.cpp are written against this
// header only, so a future backend (fibers, DR-0036) must pass them too.
//
// RULES FOR CODE THAT RUNS IN JOBS (they hold for both backends, and for a
// fiber backend if there ever is one):
//   * wait() and parallelFor() HELP: the calling thread runs other jobs while
//     it waits, so waiting never idles a core. Therefore:
//       - do NOT hold a mutex across a wait()/parallelFor() call
//       - do NOT cache thread-local state across one
//       - do NOT call OS-TLS-dependent APIs (GL contexts) around waits
//   * Main-thread work goes through onMain/pumpMain: window and GPU calls must
//     land on the real main thread.
//   * Keep job bodies coarse: this is a task scheduler, not an
//     instruction-level parallelizer. One range should be >= ~10 µs of work.
//   * BLOCKING work (file reads, decompression, anything that sleeps in the OS)
//     goes to runIo(), never run(): a blocked pool worker is a core the frame
//     does not have.
//   * PROFILER RULE: run() bodies must NOT open ENGINE_PROFILE_SCOPEs. They are
//     async and may span profiler frame boundaries, and TimerChannel's
//     beginFrame clears per-thread buffers assuming every scope closed.
//     parallelFor bodies MAY scope: it blocks, so its scopes close in the frame.
//
// Kits do NOT get this header; they reach the pool through the C API.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <utility>
#include <type_traits>
#include <new>
#include <memory>

namespace jobs {

// Opaque completion counter. Copyable; all copies refer to the same counter.
// A default-constructed handle is "already complete".
//
// LIFETIME: the handle holds a reference to the backend's job block,
// so it is safe to stash across frames — wait() on an old handle returns
// immediately once the job completed, never dereferences freed memory.
// (Audit C.5: the previous raw-pointer handle dangled after pumpMain()'s
// sweep freed completed jobs; wait() on a stashed handle was a UAF.) A held
// handle pins the job's block, so drop handles when done with them.
namespace detail {
// What a handle points at: a completion flag and a reference count, at the
// front of the backend's own job block. INTRUSIVE, so a job is one block with
// no separate control block, and a handle costs one atomic increment and one
// decrement (it was a std::shared_ptr). destroy() frees the block when the
// last reference (the queue's or a handle's) goes.
struct JobRef {
    std::atomic<uint32_t> refs{1};
    std::atomic<bool>     done{false};
    void (*destroy)(JobRef*) = nullptr;
    void retain() { refs.fetch_add(1, std::memory_order_relaxed); }
    void release() { if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) destroy(this); }
};
} // namespace detail

struct JobHandle {
    detail::JobRef* ref  = nullptr;   // the backend's job block, or null
    uint8_t         lane = 0;         // 0: the pool, 1: the I/O lane (backend use)

    JobHandle() = default;
    JobHandle(detail::JobRef* r, uint8_t l) : ref(r), lane(l) {}   // takes a reference
    JobHandle(const JobHandle& o) : ref(o.ref), lane(o.lane) { if (ref) ref->retain(); }
    JobHandle(JobHandle&& o) noexcept : ref(o.ref), lane(o.lane) { o.ref = nullptr; }
    JobHandle& operator=(const JobHandle& o) {
        if (this != &o) { if (o.ref) o.ref->retain(); if (ref) ref->release(); ref = o.ref; lane = o.lane; }
        return *this;
    }
    JobHandle& operator=(JobHandle&& o) noexcept {
        if (this != &o) { if (ref) ref->release(); ref = o.ref; lane = o.lane; o.ref = nullptr; }
        return *this;
    }
    ~JobHandle() { if (ref) ref->release(); }
    bool valid() const { return ref != nullptr; }
};

// What runs first when there is more work than workers. A worker always takes
// the highest priority it can find, its own queue first, then anyone's.
//   Frame       on this frame's critical path. The default, and what every
//               caller had before priorities existed.
//   Normal      wanted soon, but a frame must not wait behind it.
//   Background  streaming, precomputation: runs when nothing else wants a core.
enum class Priority : uint8_t { Frame = 0, Normal = 1, Background = 2 };

// Spawn the worker pool (hardware_concurrency - 1 workers + the calling
// thread participates in waits). Call once from EngineRuntime::init on the
// main thread. numWorkers == 0 picks the default.
// spinMicros: how long an idle worker keeps looking for work before it sleeps.
// Sleeping costs a wake-up (tens of microseconds) the next time work arrives;
// spinning costs power. < 0 picks the default (see jobs_native.cpp).
void init(uint32_t numWorkers = 0, int spinMicros = -1);
void shutdown();
bool initialized();

// Number of threads that may execute jobs (workers + main). Sizing hint for
// per-thread scratch arrays.
uint32_t workerCount();

// ── Fire-and-track ──────────────────────────────────────────────────────────
// Schedule fn on the pool. The returned handle completes when fn returns.
// `name` feeds the profiler; use a string literal (not copied until traced).
//
// Any callable. It is moved into the job's own block (inline up to 64 bytes;
// larger ones go to the engine heap), so run() builds no std::function and, in
// steady state, reaches no allocator. With no pool it runs inline.
namespace detail {
struct CallableOps {
    void (*invoke)(void* self);
    void (*relocate)(void* dst, void* src);   // move-construct at dst, destroy src
    void (*destroy)(void* self);
};
template <class T> struct CallableOpsFor {
    static void invoke(void* p) { (*static_cast<T*>(p))(); }
    static void relocate(void* d, void* s) { new (d) T(std::move(*static_cast<T*>(s))); static_cast<T*>(s)->~T(); }
    static void destroy(void* p) { static_cast<T*>(p)->~T(); }
    static constexpr CallableOps ops{&invoke, &relocate, &destroy};
};
// The backend's half: move the callable out of `callable` (it is left
// destroyed) into a job and queue it. Defined per backend.
JobHandle runErased(const char* name, void* callable, size_t size, size_t align,
                    const CallableOps& ops, Priority priority);
} // namespace detail

template <class F>
JobHandle run(const char* name, F&& fn, Priority priority = Priority::Frame) {
    using T = std::decay_t<F>;
    alignas(T) unsigned char storage[sizeof(T)];
    T* c = new (storage) T(std::forward<F>(fn));
    return detail::runErased(name, c, sizeof(T), alignof(T), detail::CallableOpsFor<T>::ops, priority);
}

// ── Blocking work ───────────────────────────────────────────────────────────
// Runs fn on the I/O lane: a few dedicated threads that exist to be blocked
// (file reads, decompression, a device opening). They never run pool jobs, and
// pool workers never run these, so a slow disk cannot take a core from the
// frame. The threads start on the first call. wait() on the handle helps the
// pool meanwhile.
JobHandle runIo(const char* name, std::function<void()> fn);

// ── Data parallelism ────────────────────────────────────────────────────────
// Split [0, count) across the pool; fn(begin, end) runs on ranges. BLOCKS
// until every range is done (the calling thread works too). grain is the
// minimum range size worth a task — pick it so one range is >= ~10µs of work.
//
// EXCEPTIONS: if a range throws, on any thread, no further range starts, the
// call still waits for the ranges already running, then rethrows the first
// exception to the caller. (A run() job that throws ends the process with a
// message: there is nobody to deliver it to.)
void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t begin, uint32_t end)>& fn,
                 Priority priority = Priority::Frame);

// ── Waiting ─────────────────────────────────────────────────────────────────
// Block until h completes. The calling thread executes other jobs meanwhile.
// Rules above apply: nothing thread-identity-dependent may straddle this call.
void wait(JobHandle h);

// ── Frame boundary ──────────────────────────────────────────────────────────
// Call at the top of each frame, on the main thread, before the frame's own
// main-thread work. Wakes as many sleeping workers as ran jobs in the previous
// frame, so the frame's first job does not wait for a wake-up. A frame that ran
// no jobs wakes nobody (an idle game stays idle). Optional: without it nothing
// breaks, the first job is only later.
void frameBegin();

// ── Main-thread channel ─────────────────────────────────────────────────────
// Queue fn to run on the main thread during the next pumpMain(). Safe from
// any job. The frame loop calls pumpMain() once per frame; it drains the
// queue and returns. This is facade-owned so a fiber backend needs no
// pinned-task support.
void onMain(std::function<void()> fn);
// Runs everything queued as of now and returns how many it ran. A callback that
// queues MORE work lands in the next batch, not this one (the queue is swapped
// under the lock before anything runs), which is why the count is returned and
// why drainMain() exists.
size_t pumpMain();

// ── Drain before anything a queued callback might point INTO goes away ───────
// pumpMain() empties one batch. That is not enough before unloading code: a
// kit's deferred callback is a function pointer into a dylib, so any entry
// still queued when that dylib is dlclosed becomes a jump into unmapped memory
// the next time the frame loop pumps. Call this immediately before unloading
// modules or detaching plugins.
//
// Bounded, because "drain until empty" is a hang if a callback re-queues itself
// unconditionally. Rounds are cheap (a swap and a walk); anything still queued
// after the cap is a runaway, reported and dropped — dropping work is bad, and
// calling into a freed library is worse.
void drainMain(int maxRounds = 8);

// ── Scratch memory for a job ────────────────────────────────────────────────
// Each thread has its own bump arena (engine heap, Tag::Jobs, created on first
// use). scratchAlloc is a pointer bump with no lock. Memory lasts until the
// enclosing ScratchScope ends; every run() and parallelFor range runs inside
// one, so a job just allocates and returns. Trivially destructible data only.
// Never hand scratch memory to another job or keep it past the scope.
void* scratchAlloc(size_t bytes, size_t align = alignof(std::max_align_t));
template <class T> T* scratchAlloc(size_t count = 1) {
    return static_cast<T*>(scratchAlloc(sizeof(T) * count, alignof(T)));
}
struct ScratchScope {
    ScratchScope();
    ~ScratchScope();
    ScratchScope(const ScratchScope&)            = delete;
    ScratchScope& operator=(const ScratchScope&) = delete;
private:
    size_t m_offset, m_overflow;
};

// ── What the scheduler has done since init (for tests and the profiler) ─────
struct Stats {
    uint64_t jobsRun;       // run() bodies and parallelFor tickets executed
    uint64_t steals;        // jobs taken from another thread's queue
    uint64_t parks;         // times a worker went to sleep
    uint64_t wakes;         // wake-ups asked of the OS
    uint64_t inlineRuns;    // jobs run by the submitter because a queue was full
    uint64_t ioJobs;        // jobs run on the I/O lane
    uint64_t spinNanos;     // time workers spent awake looking for work and finding
                            // none (the CPU an idle pool burns)
};
Stats stats();
const char* backendName();  // "native" (recorded in benchmark results)

} // namespace jobs
