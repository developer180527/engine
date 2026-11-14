// ── engine::jobs — the native backend (DR-0036) ─────────────────────────────
//
// A work-stealing scheduler written for this engine. STANDARD C++ ONLY: threads
// through engine::threads::start, parking through std::mutex and
// std::condition_variable, everything else atomics. There is no OS call in this
// file, so a platform layer can supply threads and parking later by replacing
// those two things and nothing else.
//
// THE SHAPE
//   * One queue set per pool thread (the main thread is slot 0, workers 1..N):
//     a lock-free deque per priority (Chase-Lev, in the form proven for weak
//     memory models by Lê, Pop, Cohen and Zappa Nardelli, 2013). The owner
//     pushes and pops at the bottom (the newest job, still warm in its cache)
//     without a lock; thieves take from the top (the oldest) with one
//     compare-and-swap. A locked queue was measured first: with one producer
//     and eleven thieves on one lock, submission cost 1.2 µs a job.
//   * Threads the engine did not start (a kit's, a provider's) have no slot.
//     Their jobs go to a few shared, locked injection queues that every worker
//     also takes from. So there is no registration and no limit on how many
//     such threads there are.
//   * A worker takes the highest priority it can find: its own queue, the
//     injection queue, then a victim's, before looking at the next priority.
//
// WAKING, which is where this backend earns its place
//   * A push wakes at most as many sleepers as it added jobs, never all of them.
//   * A worker that finds nothing keeps looking for a short time (spinMicros)
//     before it sleeps, so work that arrives within a frame finds it awake.
//   * When nothing is submitted, every worker is asleep on a condition variable
//     and the pool costs nothing (DR-0011).
//   The measurements against enkiTS are in info.md.
//
//   * Nobody is woken while a worker is already LOOKING for work (g_searching):
//     that worker will find the new job sooner than a sleeper could be woken.
//     When the last searcher finds a job and more are queued, it wakes one
//     sleeper to search in its place. (The Go scheduler's rule. Without it one
//     producer feeding short jobs paid for a wake-up on every submission:
//     800 000 of them in the benchmark.)
//
// NO LOST WAKE-UP. A sleeper increments g_sleepers and THEN reads g_pending; a
// pusher increments g_pending and THEN reads g_sleepers. Both are seq_cst, so
// at least one of them sees the other. The sleeper holds g_sleepMu from its
// check until it is inside wait(); the pusher takes and releases that mutex
// before it notifies, so the notify cannot land in between. A searcher stops
// being one (g_searching--) BEFORE that check, so a pusher that skipped the
// wake because it saw a searcher is seen by that searcher's check.
//
// parallelFor ALLOCATES NOTHING. The job lives on the caller's stack; what is
// queued are "tickets" that point at it. Whoever holds a ticket claims ranges
// from a shared counter until none are left. The caller claims too, then takes
// back whatever tickets nobody started, then waits (helping) for the rest.
#include "core/jobs/jobs.h"
#include "core/jobs/jobs_shared.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/logger.h"
#include "core/memory/mem.h"
#include "core/profiler.h"
#include "core/thread_qos.h"
#include "core/thread_start.h"

namespace jobs {
namespace {

inline void cpuRelax() { detail::relax(); }

constexpr int      kPriorities = 3;
constexpr uint32_t kRingSize   = 1024;             // per priority, per thread; power of two
constexpr uint32_t kRingMask   = kRingSize - 1;
constexpr int      kDefaultSpinMicros = 100;

struct Task {
    void (*exec)(Task*) = nullptr;
};

struct SpinLock {
    std::atomic_flag f = ATOMIC_FLAG_INIT;
    void lock() {
        int spins = 0;
        while (f.test_and_set(std::memory_order_acquire)) {
            if (++spins < 64) cpuRelax();
            else { std::this_thread::yield(); spins = 0; }   // the holder was descheduled
        }
    }
    void unlock() { f.clear(std::memory_order_release); }
};

// An injection queue: many producers, many consumers, one small lock. `size` is
// read without the lock so an empty one costs a single load.
struct alignas(64) Ring {
    SpinLock              lock;
    uint32_t              head = 0, tail = 0;      // under lock; tail - head == count
    std::atomic<uint32_t> size{0};
    Task*                 items[kRingSize];

    bool push(Task* t) {
        lock.lock();
        if (tail - head == kRingSize) { lock.unlock(); return false; }
        items[tail++ & kRingMask] = t;
        size.store(tail - head, std::memory_order_relaxed);
        lock.unlock();
        return true;
    }
    Task* popTop() {               // oldest
        if (size.load(std::memory_order_relaxed) == 0) return nullptr;
        lock.lock();
        Task* t = nullptr;
        if (tail != head) { t = items[head++ & kRingMask]; size.store(tail - head, std::memory_order_relaxed); }
        lock.unlock();
        return t;
    }
};

// The owner's queue: Chase-Lev, fixed capacity. push() and pop() are called by
// the owning thread only; steal() by anyone. A full deque refuses the push and
// the submitter runs the job itself.
struct Deque {
    alignas(64) std::atomic<int64_t> top{0};
    alignas(64) std::atomic<int64_t> bottom{0};
    alignas(64) std::atomic<Task*>   items[kRingSize];

    bool push(Task* t) {
        const int64_t b  = bottom.load(std::memory_order_relaxed);
        const int64_t tp = top.load(std::memory_order_acquire);
        if (b - tp >= (int64_t)kRingSize) return false;
        items[b & kRingMask].store(t, std::memory_order_relaxed);
        bottom.store(b + 1, std::memory_order_release);
        return true;
    }
    Task* pop() {
        const int64_t b = bottom.load(std::memory_order_relaxed) - 1;
        bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t tp = top.load(std::memory_order_relaxed);
        Task* t = nullptr;
        if (tp <= b) {
            t = items[b & kRingMask].load(std::memory_order_relaxed);
            if (tp == b) {   // the last one: race the thieves for it
                if (!top.compare_exchange_strong(tp, tp + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
                    t = nullptr;
                bottom.store(b + 1, std::memory_order_relaxed);
            }
        } else {
            bottom.store(b + 1, std::memory_order_relaxed);
        }
        return t;
    }
    // nullptr when empty, and also when another thief won the race: the caller
    // looks elsewhere and comes back, which g_pending makes it do.
    Task* steal() {
        int64_t tp = top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const int64_t b = bottom.load(std::memory_order_acquire);
        if (tp >= b) return nullptr;
        Task* t = items[tp & kRingMask].load(std::memory_order_relaxed);
        if (!top.compare_exchange_strong(tp, tp + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            return nullptr;
        return t;
    }
};

constexpr uint32_t kInjectStripes = 4;   // injection queues per priority, picked per thread

struct alignas(64) Slot {
    Deque    deques[kPriorities];
    // Counters: written by the owning thread only, read by stats().
    std::atomic<uint64_t> jobsRun{0}, steals{0}, parks{0}, inlineRuns{0};
    uint32_t rng = 0x9E3779B9u;
};

// ── State ───────────────────────────────────────────────────────────────────
std::atomic<bool>        g_init{false};
std::atomic<bool>        g_stop{false};
std::thread::id          g_mainThread;
Slot*                    g_slots = nullptr;        // [0] main, [1..] workers
uint32_t                 g_slotCount = 0;
Ring*                    g_inject = nullptr;       // [kInjectStripes * kPriorities], for threads with no slot
std::vector<std::thread> g_threads;
int                      g_spinMicros = kDefaultSpinMicros;

std::atomic<int>         g_pending{0};             // queued and not yet taken, all queues
std::atomic<int>         g_sleepers{0};
std::atomic<int>         g_searching{0};           // workers awake with nothing to run
std::mutex               g_sleepMu;
std::condition_variable  g_sleepCv;
std::atomic<uint64_t>    g_wakes{0};
std::atomic<uint64_t>    g_foreignRun{0}, g_foreignSteals{0}, g_foreignInline{0};

// A generation, so a thread that had a slot in an earlier init() does not use
// that slot number against a new pool (init, shutdown, init in one process).
std::atomic<uint32_t>    g_generation{0};
thread_local int         t_slot = -1;
thread_local uint32_t    t_slotGeneration = 0;

inline int mySlot() {
    return t_slotGeneration == g_generation.load(std::memory_order_relaxed) ? t_slot : -1;
}

// n new jobs were queued: wake sleepers for the ones no searcher will take.
void wake(int n) {
    const int need = n - g_searching.load(std::memory_order_seq_cst);
    if (need > 0) {
        const int asleep = g_sleepers.load(std::memory_order_seq_cst);
        if (asleep > 0) {
            { std::lock_guard<std::mutex> lk(g_sleepMu); }   // see NO LOST WAKE-UP
            if (need >= asleep) g_sleepCv.notify_all();
            else for (int i = 0; i < need; ++i) g_sleepCv.notify_one();
            g_wakes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    detail::notifyWaiters();   // a thread parked in wait() can run this too
}

// Queue t. False if the queue was full: the caller runs it itself.
std::atomic<uint32_t> g_stripeCounter{0};
inline uint32_t myStripe() {
    static thread_local uint32_t s = g_stripeCounter.fetch_add(1, std::memory_order_relaxed) % kInjectStripes;
    return s;
}

bool push(Task* t, Priority pr, int slot) {
    const bool ok = slot >= 0 ? g_slots[slot].deques[(int)pr].push(t)
                              : g_inject[myStripe() * kPriorities + (int)pr].push(t);
    if (!ok) return false;
    g_pending.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

Task* findTask(int slot) {
    if (g_pending.load(std::memory_order_relaxed) <= 0) return nullptr;
    uint32_t seed;
    if (slot >= 0) { uint32_t& s = g_slots[slot].rng; s ^= s << 13; s ^= s >> 17; s ^= s << 5; seed = s; }
    else {
        static thread_local uint32_t s = 0x85EBCA6Bu;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; seed = s;
    }
    for (int p = 0; p < kPriorities; ++p) {
        Task* t = nullptr;
        if (slot >= 0) t = g_slots[slot].deques[p].pop();
        for (uint32_t i = 0; i < kInjectStripes && !t; ++i)
            t = g_inject[((seed + i) % kInjectStripes) * kPriorities + p].popTop();
        if (!t) {
            for (uint32_t i = 0; i < g_slotCount && !t; ++i) {
                const uint32_t v = (seed + i) % g_slotCount;
                if ((int)v == slot) continue;
                t = g_slots[v].deques[p].steal();
            }
            if (t) {
                if (slot >= 0) g_slots[slot].steals.fetch_add(1, std::memory_order_relaxed);
                else           g_foreignSteals.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (t) {
            g_pending.fetch_sub(1, std::memory_order_seq_cst);
            return t;
        }
    }
    return nullptr;
}

inline void runTask(Task* t, int slot) {
    if (slot >= 0) g_slots[slot].jobsRun.fetch_add(1, std::memory_order_relaxed);
    else           g_foreignRun.fetch_add(1, std::memory_order_relaxed);
    t->exec(t);
}

void workerMain(uint32_t slot, uint32_t generation) {
    t_slot = (int)slot;
    t_slotGeneration = generation;
    using clock = std::chrono::steady_clock;
    bool searching = false;
    clock::time_point searchStart;
    int rounds = 0;
    for (;;) {
        if (Task* t = findTask((int)slot)) {
            if (searching) {
                searching = false;
                // The last searcher is about to be busy: if more is queued,
                // someone has to keep looking.
                if (g_searching.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
                    g_pending.load(std::memory_order_seq_cst) > 0)
                    wake(1);
            }
            runTask(t, (int)slot);
            continue;
        }
        if (g_stop.load(std::memory_order_acquire) && g_pending.load(std::memory_order_acquire) <= 0) break;

        // Look for a while before sleeping. The clock is read every 32 rounds,
        // not every round: reading it is the expensive part of an empty loop.
        if (g_spinMicros > 0) {
            if (!searching) {
                searching = true;
                g_searching.fetch_add(1, std::memory_order_seq_cst);
                searchStart = clock::now();
                rounds = 0;
            }
            ++rounds;
            if ((rounds & 31) != 0) { cpuRelax(); continue; }
            if (clock::now() - searchStart < std::chrono::microseconds(g_spinMicros)) { cpuRelax(); continue; }
        }
        if (searching) { searching = false; g_searching.fetch_sub(1, std::memory_order_seq_cst); }

        std::unique_lock<std::mutex> lk(g_sleepMu);
        g_sleepers.fetch_add(1, std::memory_order_seq_cst);
        if (g_pending.load(std::memory_order_seq_cst) <= 0 && !g_stop.load(std::memory_order_acquire)) {
            g_slots[slot].parks.fetch_add(1, std::memory_order_relaxed);
            g_sleepCv.wait(lk);
        }
        g_sleepers.fetch_sub(1, std::memory_order_seq_cst);
    }
    if (searching) g_searching.fetch_sub(1, std::memory_order_seq_cst);
}

// ── run() ───────────────────────────────────────────────────────────────────
// One block per job, from a per-thread free list (the engine heap, Tag::Jobs,
// only when the list is empty), holding the shared_ptr control block and the
// job together. In steady state run() does not reach the allocator.
struct BlockCache {
    static constexpr int kMax = 256;
    void*  head = nullptr;
    int    count = 0;
    size_t blockSize = 0;
    ~BlockCache() { while (head) { void* n = *(void**)head; mem::free(head); head = n; } }
};
thread_local BlockCache t_blocks;

// The second level. A producer allocates and a worker frees, so one-level
// per-thread lists drain on one side and overflow on the other. A full local
// list hands half of itself here, and an empty one takes a batch back.
struct BlockDepot {
    SpinLock         lock;
    void*            head = nullptr;      // under lock
    std::atomic<int> count{0};            // read without it, to skip an empty depot
    size_t           blockSize = 0;       // under lock
};
BlockDepot g_depot;
constexpr int kDepotMax = 4096;

template <class T>
struct BlockAlloc {
    using value_type = T;
    BlockAlloc() = default;
    template <class U> BlockAlloc(const BlockAlloc<U>&) {}
    T* allocate(size_t n) {
        const size_t bytes = n * sizeof(T);
        BlockCache& c = t_blocks;
        if (n == 1) {
            if (!c.head && g_depot.count.load(std::memory_order_relaxed) > 0) {   // refill a batch
                g_depot.lock.lock();
                if (g_depot.blockSize == bytes)
                    for (int i = 0; i < 64 && g_depot.head; ++i) {
                        void* p = g_depot.head; g_depot.head = *(void**)p;
                        g_depot.count.fetch_sub(1, std::memory_order_relaxed);
                        *(void**)p = c.head; c.head = p; ++c.count;
                    }
                g_depot.lock.unlock();
                c.blockSize = bytes;
            }
            if (c.head && c.blockSize == bytes) {
                void* p = c.head; c.head = *(void**)p; --c.count;
                return (T*)p;
            }
        }
        return (T*)mem::alloc(bytes < sizeof(void*) ? sizeof(void*) : bytes, alignof(T) < 16 ? 16 : alignof(T), mem::Tag::Jobs);
    }
    void deallocate(T* p, size_t n) {
        const size_t bytes = n * sizeof(T);
        BlockCache& c = t_blocks;
        if (n == 1 && (c.blockSize == 0 || c.blockSize == bytes)) {
            c.blockSize = bytes;
            *(void**)p = c.head; c.head = p; ++c.count;
            if (c.count >= BlockCache::kMax) {   // hand half to the depot
                g_depot.lock.lock();
                if (g_depot.blockSize == 0) g_depot.blockSize = bytes;
                while (c.count > BlockCache::kMax / 2) {
                    void* q = c.head; c.head = *(void**)q; --c.count;
                    if (g_depot.blockSize == bytes && g_depot.count.load(std::memory_order_relaxed) < kDepotMax) {
                        *(void**)q = g_depot.head; g_depot.head = q;
                        g_depot.count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_depot.lock.unlock(); mem::free(q); g_depot.lock.lock();
                    }
                }
                g_depot.lock.unlock();
            }
            return;
        }
        mem::free(p);
    }
    template <class U> bool operator==(const BlockAlloc<U>&) const { return true; }
    template <class U> bool operator!=(const BlockAlloc<U>&) const { return false; }
};

struct RunJob final : Task, detail::JobState {
    std::function<void()>   fn;
    std::shared_ptr<RunJob> self;   // keeps the block alive while it is queued

    static void execute(Task* t) {
        RunJob* j = static_cast<RunJob*>(t);
        {
            ScratchScope scratch;
            j->fn();
        }
        j->fn = nullptr;                                // captures die on the thread that ran them
        std::shared_ptr<RunJob> keep = std::move(j->self);
        j->done.store(true, std::memory_order_release);
        detail::notifyWaiters();
    }   // `keep` goes: the block is freed here unless a handle still holds it
};

// ── parallelFor ─────────────────────────────────────────────────────────────
struct ForJob final : Task {
    const std::function<void(uint32_t, uint32_t)>* fn = nullptr;
    const char*           name = nullptr;
    uint32_t              count = 0, chunk = 1;
    std::atomic<uint32_t> next{0};
    std::atomic<int>      tickets{0};    // queued or running, not counting the caller

    void claim() {
        ENGINE_PROFILE_SCOPE(name);
        for (;;) {
            const uint32_t b = next.fetch_add(chunk, std::memory_order_relaxed);
            if (b >= count) return;
            ScratchScope scratch;
            (*fn)(b, b + chunk < count ? b + chunk : count);
        }
    }
    static void execute(Task* t) {
        ForJob* j = static_cast<ForJob*>(t);
        j->claim();
        // After this decrement the caller may return and the job (on its stack)
        // is gone: nothing below may touch `j`.
        if (j->tickets.fetch_sub(1, std::memory_order_acq_rel) == 1) detail::notifyWaiters();
    }
};

} // namespace

// ── What jobs_shared.cpp needs from a backend ───────────────────────────────
namespace detail {
bool tryRunOne() {
    if (!g_init.load(std::memory_order_acquire)) return false;
    const int slot = mySlot();
    Task* t = findTask(slot);
    if (!t) return false;
    runTask(t, slot);
    return true;
}
bool hasPending() { return g_pending.load(std::memory_order_seq_cst) > 0; }
int  spinMicros() { return g_spinMicros; }
} // namespace detail

const char* backendName() { return "native"; }

void init(uint32_t numWorkers, int spinMicros) {
    if (g_init.load()) return;
    g_mainThread = std::this_thread::get_id();
    g_spinMicros = spinMicros < 0 ? kDefaultSpinMicros : spinMicros;
    g_stop.store(false);
    g_pending.store(0);
    g_searching.store(0);

    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 2;
    const uint32_t workers = numWorkers > 0 ? numWorkers : (hw > 1 ? hw - 1 : 1);
    g_slotCount = workers + 1;

    g_slots = static_cast<Slot*>(mem::alloc(sizeof(Slot) * g_slotCount, alignof(Slot), mem::Tag::Jobs));
    for (uint32_t i = 0; i < g_slotCount; ++i) { new (&g_slots[i]) Slot(); g_slots[i].rng = 0x9E3779B9u * (i + 1); }
    g_inject = static_cast<Ring*>(mem::alloc(sizeof(Ring) * kInjectStripes * kPriorities, alignof(Ring), mem::Tag::Jobs));
    for (uint32_t p = 0; p < kInjectStripes * kPriorities; ++p) new (&g_inject[p]) Ring();

    const uint32_t generation = g_generation.fetch_add(1) + 1;
    t_slot = 0;
    t_slotGeneration = generation;

    g_threads.reserve(workers);
    for (uint32_t i = 1; i <= workers; ++i)
        g_threads.push_back(engine::threads::start(engine::qos::Class::Initiated,
                                                   [i, generation] { workerMain(i, generation); }));
    detail::startShared();
    g_init.store(true);
    LOG_INFO("Jobs", "worker pool up: %u task threads (workers + main), spawned once", g_slotCount);
}

void shutdown() {
    if (!g_init.load()) return;
    // The I/O lane first, while the pool can still take what its jobs submit.
    detail::shutdownShared();
    // Everything queued still runs: workers leave only once nothing is pending,
    // and this thread helps them get there.
    while (detail::tryRunOne()) {}
    g_stop.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_sleepMu); }
    g_sleepCv.notify_all();
    for (std::thread& t : g_threads) t.join();
    g_threads.clear();
    // A job that was running when the workers looked may have queued one more
    // after they left: it runs here, so no job is ever dropped.
    while (detail::tryRunOne()) {}
    g_init.store(false);
    {   // the block depot goes back to the heap
        g_depot.lock.lock();
        while (g_depot.head) { void* n = *(void**)g_depot.head; mem::free(g_depot.head); g_depot.head = n; }
        g_depot.count.store(0); g_depot.blockSize = 0;
        g_depot.lock.unlock();
    }

    for (uint32_t i = 0; i < g_slotCount; ++i) g_slots[i].~Slot();
    for (uint32_t p = 0; p < kInjectStripes * kPriorities; ++p) g_inject[p].~Ring();
    mem::free(g_slots);  g_slots = nullptr;
    mem::free(g_inject); g_inject = nullptr;
    g_slotCount = 0;
    g_generation.fetch_add(1);   // this thread's slot 0 is no longer a slot
    LOG_INFO("Jobs", "worker pool down");
}

bool initialized() { return g_init.load(); }

uint32_t workerCount() { return g_init.load() ? g_slotCount : 1u; }

JobHandle run(const char* /*name*/, std::function<void()> fn, Priority priority) {
    // No pool (headless tools, early boot): run inline. A default handle is
    // "already complete", so callers' wait() is still correct.
    if (!g_init.load(std::memory_order_acquire)) { fn(); return {}; }

    std::shared_ptr<RunJob> job = std::allocate_shared<RunJob>(BlockAlloc<RunJob>());
    job->exec = &RunJob::execute;
    job->fn   = std::move(fn);
    job->self = job;
    JobHandle h;
    h.opaque = std::shared_ptr<detail::JobState>(job, static_cast<detail::JobState*>(job.get()));

    const int slot = mySlot();
    if (push(job.get(), priority, slot)) {
        wake(1);
    } else {
        // The queue is full: the submitter runs it. The work still happens, and
        // a producer that outruns the pool is slowed down, which is the point.
        if (slot >= 0) g_slots[slot].inlineRuns.fetch_add(1, std::memory_order_relaxed);
        else           g_foreignInline.fetch_add(1, std::memory_order_relaxed);
        RunJob::execute(job.get());
    }
    return h;
}

void parallelFor(const char* name, uint32_t count, uint32_t grain,
                 const std::function<void(uint32_t, uint32_t)>& fn, Priority priority) {
    if (count == 0) return;
    const uint32_t g = grain > 0 ? grain : 1;
    // Not worth a dispatch (or no pool): run the whole range inline.
    if (!g_init.load(std::memory_order_acquire) || count <= g) { fn(0, count); return; }

    ForJob job;
    job.exec  = &ForJob::execute;
    job.fn    = &fn;
    job.name  = name;
    job.count = count;
    // Ranges of at least `grain`, and about 8 per thread when the work is big,
    // so threads that finish early have something left to take.
    const uint32_t even = count / (g_slotCount * 8u);
    job.chunk = even > g ? even : g;
    const uint32_t chunks = (count + job.chunk - 1) / job.chunk;

    // One ticket per other thread that could usefully join, never more than
    // there are ranges beyond the caller's own.
    const int slot = mySlot();
    uint32_t want = chunks - 1;
    if (want > g_slotCount - 1) want = g_slotCount - 1;
    int pushed = 0;
    job.tickets.store((int)want, std::memory_order_relaxed);
    if (slot >= 0) {
        // Counted once for all of them: every shared counter a ticket touches
        // is a cache line eleven other cores are reading.
        Deque& d = g_slots[slot].deques[(int)priority];
        for (uint32_t i = 0; i < want; ++i) {
            if (!d.push(&job)) break;
            ++pushed;
        }
        if (pushed) g_pending.fetch_add(pushed, std::memory_order_seq_cst);
    } else {
        for (uint32_t i = 0; i < want; ++i) {
            if (!push(&job, priority, slot)) break;
            ++pushed;
        }
    }
    if (pushed < (int)want) job.tickets.fetch_sub((int)want - pushed, std::memory_order_relaxed);
    if (pushed > 0) wake(pushed);

    job.claim();

    // Take back the tickets nobody started: they are the newest entries in this
    // thread's own queue. Anything else found there on the way is a job like any
    // other, and is run. Then help until the tickets that were taken finish.
    if (slot >= 0) {
        Deque& d = g_slots[slot].deques[(int)priority];
        while (job.tickets.load(std::memory_order_acquire) > 0) {
            Task* t = d.pop();
            if (!t) break;
            g_pending.fetch_sub(1, std::memory_order_seq_cst);
            if (t == &job) job.tickets.fetch_sub(1, std::memory_order_acq_rel);
            else           runTask(t, slot);
        }
    }
    detail::helpUntil([&job] { return job.tickets.load(std::memory_order_acquire) <= 0; });
}

void wait(JobHandle h) {
    if (!h.valid()) return;
    auto* s = static_cast<detail::JobState*>(h.opaque.get());
    detail::helpUntil([s] { return s->done.load(std::memory_order_acquire); });
}

bool detail::onMainThread() {
    return !g_init.load() || std::this_thread::get_id() == g_mainThread;
}
void detail::backendSweep() {}   // run() blocks free themselves

Stats stats() {
    Stats s{};
    for (uint32_t i = 0; i < g_slotCount; ++i) {
        s.jobsRun    += g_slots[i].jobsRun.load(std::memory_order_relaxed);
        s.steals     += g_slots[i].steals.load(std::memory_order_relaxed);
        s.parks      += g_slots[i].parks.load(std::memory_order_relaxed);
        s.inlineRuns += g_slots[i].inlineRuns.load(std::memory_order_relaxed);
    }
    s.jobsRun    += g_foreignRun.load(std::memory_order_relaxed);
    s.steals     += g_foreignSteals.load(std::memory_order_relaxed);
    s.inlineRuns += g_foreignInline.load(std::memory_order_relaxed);
    s.wakes       = g_wakes.load(std::memory_order_relaxed);
    s.ioJobs      = detail::ioJobsRun();
    return s;
}

} // namespace jobs
