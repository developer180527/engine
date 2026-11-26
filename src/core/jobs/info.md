---
status: as-built
tier: hardened
verified: 2026-11-27
covers:
  - src/core/jobs/
tests:
  - tests/jobs_test.cpp
  - tests/stress_jobs.cpp
  - tests/jobs_fuzz_test.cpp
  - tests/jobs_torture_test.cpp
  - tests/thread_qos_test.cpp
---
# engine::jobs — the engine-wide task scheduler

One worker pool for the whole engine, spawned once at `EngineRuntime::init`
(`jobs::init`), torn down in `shutdown()`. Thread creation is a boot event,
never a frame event — this subsystem exists so the frame loop makes no
threading syscalls (workers spin-then-park only when truly idle).

## The scheduler (2026-11-14, DR-0036)

`jobs.h` has one backend, `jobs_native.cpp`: the engine's own work-stealing
scheduler, standard C++ only (no OS call), any number of foreign threads with
no registration. enkiTS was the backend until 2026-11-14 and was kept beside it
for comparison; with the comparison made (the table below) it was removed the
same day (WO-079: the submodule, its patch, `jobs_enkits.cpp` and the
`ENGINE_JOBS_BACKEND` option; a stale `enkits` cache entry is refused at
configure time). The comparison numbers stay below as the record.

In `jobs_shared.cpp`, apart from the scheduler: the main-thread queue, the I/O
lane, per-thread scratch, and waiter parking. The job system is compiled into
`engine_core` (it was in `engine_runtime` while living in `src/core`).

**How the native backend works**
- **Queues:** one lock-free deque (Chase-Lev) per pool thread and priority.
  The owner pushes and pops the newest job without a lock; thieves take the
  oldest with one compare-and-swap. Threads the engine did not start push to a
  few shared, locked injection queues.
- **Priorities:** `Frame`, `Normal`, `Background`. A worker takes the highest
  it can find anywhere before looking lower. Every caller that passes none gets
  `Frame`, which is what all of them had before.
- **Waking:** a push wakes only as many sleepers as it added jobs, and nobody
  while another worker is already looking for work. An idle worker looks for
  10 µs; after that only two keep looking, for up to `spinMicros` (100 µs), and
  the rest sleep. `jobs::frameBegin()`, called by the runtime at the top of
  each frame, wakes as many workers as ran jobs last frame, so a frame's first
  job does not wait for a wake-up (a frame with no jobs wakes nobody).
- **No shared count of queued jobs.** A worker about to sleep announces itself
  and then looks at every queue; a pusher publishes its job and then reads the
  sleeper count. A seq_cst fence on each side means one of them sees the other.
  (There was a counter that every push and every take wrote, one cache line for
  all cores.)
- **`parallelFor` allocates nothing:** the job is on the caller's stack, and
  what is queued are tickets pointing at it. Ticket holders claim ranges from a
  shared counter. The caller claims too, takes back unstarted tickets, then
  helps until the rest finish.
- **`run`:** a template that takes any callable, move-only ones included. The
  callable is moved into the job's own block (inline up to 64 bytes, else the
  engine heap), the block comes from a two-level free list (per thread, then a
  shared depot), and `JobHandle` holds an intrusive reference count on it: no
  `std::function`, no `shared_ptr`, and in steady state no allocation
  (`allocs.steady` reads 0.000 per job).
- **Waiters** (`wait`, `parallelFor`'s caller) help, spin briefly, then sleep
  until a job finishes or one is queued. No timeout: the same fenced handshake
  as the workers, against `notifyWaiters`.
- **The I/O lane (`runIo`):** 2 to 4 threads that exist to be blocked, started
  on first use. The asset loader and the audio device start use it; they were
  on the pool, where a blocked read took a core from the frame.
- **Scratch (`scratchAlloc`, `ScratchScope`):** a bump arena per thread, from
  the engine heap (`Tag::Jobs`), created on first use. Every job runs inside a
  scope, and scopes nest, so a job that runs other jobs while it waits keeps
  its own scratch.

**Measured** (`tests/jobs_bench.cpp`, Release, Apple M-series, 12 threads,
medians; three interleaved runs of each backend agreed to about 15%):

| | enkiTS | native |
|---|---|---|
| `run` from one thread | 2 500 to 10 000 ns/job | 550 ns/job |
| `run` from 4 threads | 450 to 1 570 ns/job | 320 to 380 ns/job |
| `parallelFor`, 64 items of ~2.5 µs | 37 to 40 µs (2.2x) | 16.5 to 18 µs (4.8x) |
| `parallelFor`, 4M floats | 0.28 to 0.31 ms | 0.25 to 0.37 ms |
| a frame (6 `parallelFor`s + 200 jobs), back to back | 1.9 to 3.3 ms | 0.15 to 0.28 ms |
| the same frame every 4 ms, median | 2.03 ms | 0.54 ms |
| CPU over that paced run | 138% of a core | 67% of a core |
| CPU in 500 ms idle | 0.02 ms | 0.02 ms |

The gap is the wake policy: enkiTS wakes every sleeping worker on every
submission. The large `parallelFor` is level because there the work dominates.
`spinMicros` was swept from 0 to 500: 0 loses the small `parallelFor` (1.9x),
200 and up burn CPU (108% to 149%) for nothing, 50 to 100 are equal; 100 is the
default. **Not measured:** Windows, Linux, phones, or a machine with fewer
cores than work.

**Proven by**
- `jobs_fuzz_test`: random use of every entry point from engine and foreign
  threads, with pool size and spin varied per seed, checked exactly (every body
  once, every `parallelFor` index once, waits honest, scratch intact, `onMain`
  on the main thread), plus a liveness check where nobody helps. 60 seeds in
  the unit lane, 1 500 in the stress lane. It passes on both backends.
- ThreadSanitizer: `jobs_test` and 190 fuzz seeds, no report.
- A watchdog fails the fuzz run if nothing progresses for 20 s, naming the
  phase, and a waiter-liveness phase (a thread in `wait` with one worker) runs
  after the worker one. A planted lost notify in `notifyWaiters` hung the run
  before the watchdog existed; with it, it is reported in 20 s.
- Two planted bugs, both caught: a thief keeping a job it lost the race for
  (crash), and a worker sleeping without re-checking for work (the liveness
  check; the random rounds alone did not see it, and neither did liveness with
  three workers, which is why it runs with one).
- `stress_jobs` (300 rounds) and the whole unit lane on the native backend.

**The 2026-11-14 review** (Mac, after the review's fixes; same bench, medians
of four runs):

| | before | after |
|---|---|---|
| `run` from one thread | 433 ns/job | 377 ns/job (the shared counter's cost) |
| `run` from 4 threads | 316 to 350 ns/job | 200 to 237 ns/job |
| allocations per `run`, steady state | 1 (`std::function` + control block) | 0 |
| spinning on paced frames | 20 to 23% of a core | 7 to 8% |
| first job queued to a worker starting it, paced | 20 µs | 1.5 to 1.9 µs with `frameBegin` |
| frames in which no worker took part | | 0 of 1 800 |

`pfor.small` stayed 4.4x to 5.3x throughout. The paced frame's median moves
±15% between runs on this machine, more than any of these changes, so it is
not the number to judge them by; `first_worker` and `spin` are.

**Torture test (2026-11-26).** `tests/jobs_torture_test.cpp`, one case per edge:
a count near 2^32, a flood of 1 000 000 `run()`s, a 317 810-job recursive tree
(every node a `run` and a `wait`), 4-deep nested `parallelFor`, 3x as many CPU
hogs as cores, 2 000 short-lived foreign threads, 200 init/shutdown cycles with
work queued, wake latency, memory over 10 000 000 jobs, priority under
contention, and exceptions. It found three defects (BUG-0073, BUG-0074,
BUG-0075), all fixed. Since: all cases pass on macOS arm64 (release, TSan,
ASan+UBSan) and Linux arm64 (GCC release and GCC TSan, in a container), and a
20-minute loop of the full suite plus 200 fresh fuzz seeds per iteration ran 126
iterations with no failure. The fixes measured against the commit before them,
interleaved, 7 runs each: every benchmark within noise.

**Wake latency is the OS's, not the scheduler's.** A job to a fully parked pool
starts after p50 15 to 18 µs on macOS with a tail of several milliseconds, and a
bare `std::condition_variable` with no job system shows the same tail (p99 100 to
230 µs, worst 1.5 to 5.9 ms). On Linux arm64: p50 24 µs, p99 57 µs, worst 0.8 ms.
The defences are the idle spin and `frameBegin`, which keep workers awake across
a frame instead of waking them on its critical path.
So the torture test's `latency` case judges what is the scheduler's: no lost
wake-up (every job starts within 2 s; a dropped notify fails it by name in
about 6 s, mutation-checked) and a median under 5 ms. It prints the tail but does
not judge it: on a GitHub macOS runner one run showed p99 23 ms and worst 97 ms
(2026-11-27), which is the shared VM's scheduler. A tail regression is measured on
the home server.

**Known limits**
- `findTask` checks every queue before every steal attempt (`anyQueued`): reads
  of one cache line per thread per priority. Fine at 12 to 16 threads; on 32 to
  64 it is cache traffic every idle round. Not measured on such a machine.
- One steal takes one job. A single producer of many small `run()` jobs has
  every thief racing on its queue; stealing half would cut that.
- `parallelFor` takes a `const std::function&`, so each call builds a
  `std::function` (which may allocate for large captures). `run()` already takes
  any callable without one.
- `scratchAlloc` outside any `ScratchScope` (main-thread code outside a job) is
  never rewound: past the arena it allocates and keeps every block.
- Calling into the job system from a thread while another thread runs
  `shutdown()` is undefined: nothing guards it.
- Helping in `wait` runs other jobs on the waiter's stack, as enkiTS did: deep
  chains of waits inside jobs can run a worker's stack out.
- `Background` work can wait forever while `Frame` work saturates the pool.
- A frame that calls `frameBegin` and then does more than `spinMicros` of
  main-thread work before its first job gets nothing from it: the woken
  workers have gone back to sleep.
- Workers are not pinned and stealing ignores topology (CCX/cluster); DR-0036
  has why. Revisit with measurements from a machine where it matters.

**The sections below were written for the enkiTS backend, removed on
2026-11-14.** They are kept for what they found (BUG-0003, BUG-0056, the QoS
measurement, the fiber contract); the code they cite is gone. What still holds
for the native backend is said in each.

## External threads must claim a slot (BUG-0003)

Threads the engine did not spawn — kit threads, and the audio provider's, which
the ABI explicitly hands `parallelFor` and tells to decode on — must be
registered with the backend before they touch it. enkiTS indexes per-thread state
by thread number and returns **0** for any thread it does not know, which is the
same slot as the **main thread**. Two threads driving one slot corrupt the
per-thread pipe.

The symptom was a **livelock**, it reproduced about one run in five *under TSan
and never without it*, and the fix is `ensureThreadRegistered()`: a
`thread_local` slot claimed on first use and released on thread exit, guarded on
`g_init` so a thread outliving `shutdown()` cannot touch a dead scheduler. When
no slot is available the call runs **inline** — slower for that caller, correct
for everyone — rather than falling back to slot 0.

This is a hard constraint on the FTL swap and not an enkiTS quirk to leave
behind: a fiber scheduler needs a thread to be fiber-ready before it can run or
wait on tasks, so *"any thread may call `jobs::run`"* is a facade promise the new
backend has to keep deliberately rather than inherit. `stress_jobs` case 2 is
that promise, tested.

## `run()` registers AFTER queueing, not before (BUG-0056)

The order of these two statements in `jobs::run` is load-bearing:

```cpp
g_ts.AddTaskSetToPipe(task.get());     // FIRST
{ lock; g_inflight.push_back(task); }  // then register
```

`g_inflight` exists to keep a pending block alive for callers who discard the
handle — which is most of them. `pumpMain()` sweeps it by asking
`GetIsComplete()`, and in enkiTS that is `m_RunningCount == 0` where the counter
**starts at zero**. So a task that has been constructed but not yet queued reads
as *complete*.

Registering first therefore admitted a block in a state where the sweep's test
was meaningless, and a sweep landing between the two statements dropped the
registry's reference to a job that had not run yet. The caller then discarded
the handle, the refcount hit zero, and the block was freed while sitting in the
pipe — `ENKI_ASSERT(GetIsComplete())` in `~ICompletable`, or a use-after-free
with asserts off.

**`run()` is also not always asynchronous**, which is the related thing to know
before using it: `SplitAndAddTask` executes a task inline on the submitting
thread when that thread's pipe is full. `JoltJobsAdapter` has to defer around
that (BUG-0055), and any caller that must not re-enter itself does too.

## Workers are classified for the OS scheduler

`init()` installs `cfg.profilerCallbacks.threadStart`, which enkiTS calls **on
each new worker** as its first act inside `TaskingThreadFunction`. It sets
`engine::qos::Class::Initiated` — one step below the main thread's
`USER_INTERACTIVE`, because eleven threads all claiming the top class is how a
process starves its own main thread.

This is necessary rather than decorative: **`pthread_create` does not inherit the
creating thread's class.** It produces `QOS_CLASS_DEFAULT` (21), a real value one
step *below* the main thread's 33, so without the hook the entire pool ran
demoted by omission. Measured at a 14.5x per-thread advantage under contention;
invisible on an idle developer machine and material on a four-core laptop.

Two constraints on any backend swap:

* the hook must run **on** the worker thread, not on the spawner — every
  platform primitive underneath is self-scoped;
* **external threads must keep their owner's class.** A registered kit or
  provider thread never enters `TaskingThreadFunction`, so it is never
  reclassified, and `thread_qos_test` §4 pins that. Silently retuning somebody
  else's decode thread is the same action-at-a-distance the external-slot design
  exists to avoid.

`SafeCallback` in enkiTS is a plain null check with no profiling-build guard, so
this fires in every configuration — verified in
`third_party/enkiTS/src/TaskScheduler.cpp:258`.

## Backend & the FTL swap contract
Backend: **native** (`jobs_native.cpp`; until 2026-11-14 enkiTS). The facade
(`jobs.h`) is written so **FiberTaskingLib** could replace it with one sibling
.cpp:

**What FTL would and would not change.** Worth stating precisely, because it is
easy to get backwards:

- **Work stealing is NOT the difference — enkiTS already steals.**
  `TaskScheduler::TryRunTask` reads its own pipe front, then walks every other
  thread's pipe back-to-front, and keeps a `hintPipeToCheck` so a thread that
  stole successfully looks there first next time. `stress_jobs` case 4 measures
  the resulting distribution rather than arguing about it (12 of 20 threads on a
  20-thread pool).
- **The difference is what a WAIT does.** enkiTS *helps*: the waiting thread runs
  other tasks on its own stack, so a nested wait grows the stack and the
  continuation always resumes on the thread that blocked. FTL *suspends*: the
  fiber yields, the thread picks up unrelated work, and the continuation may
  resume on a different thread with a fresh stack. That is the whole content of
  the call-site rules below, and it is why Phase H #34's trigger is "job graphs
  deep enough that **blocking waits dominate**" — not "we need stealing".

- `JobHandle` has **counter semantics** (wait-until-zero), which both
  enkiTS (`ICompletable`) and FTL (`AtomicCounter`) implement.
- `wait()`/`parallelFor()` **help** — the caller executes other work while
  waiting. Under fibers the continuation may resume on a different OS
  thread, hence the call-site rules (enforced by review, not the compiler):
  no thread-local caching, no mutex held, no thread-identity API across a
  wait.
- The **main-thread channel** (`onMain`/`pumpMain`, drained at the top of
  `tickSystems`) is facade-owned instead of using backend pinned tasks —
  fiber schedulers have no pinned-task notion, and bgfx/GLFW must land on
  the real main thread.
- `run()` handles are swept once complete at the next `pumpMain()` — wait on
  them (or drop them) within the frame that scheduled them; never stash a
  handle across frames.

Everything degrades gracefully without `init()` (headless tools, tests):
`run`/`parallelFor` execute inline on the caller.

## Tests, and why `hardened`

| test | lane | what it holds |
|---|---|---|
| `jobs_test` | unit | the FUNCTIONAL contract on a quiet pool: run/wait, a handle stashed across sweeps (audit C.5, previously a UAF), the default handle, parallelFor coverage, onMain ordering, drainMain, handles outliving shutdown |
| `stress_jobs` | stress | the same facade under CONTENTION: sustained churn, external threads (BUG-0003), nested waits, work distribution, exact parallelFor coverage on a saturated pool, and the main-thread channel under load |

`stress_jobs` is written against `jobs.h` and **never against enkiTS** — it reads
no thread number, counts no workers against `hardware_concurrency`, and never
assumes a wait runs work on the waiting thread. That makes it the **acceptance
gate for the FTL swap**: a backend that passes it is behaviourally compatible,
and one that fails names the property it broke instead of surfacing as a hang in
animation three weeks later.

It carries `test_watchdog`, because the headline failure here is a livelock and a
livelock's natural symptom is ctest killing the test at 600 s with no output. The
watchdog fires first and names the phase.

Default run is a CI smoke size (~0.5 s). `./stress_jobs 5000` is the soak.

## Current schedulers-through-the-pool
- **AnimatorSystem** — serial collect pass (query walk + context-map
  inserts), then `parallelFor("anim.sample")` over entities; each entity's
  ozz sampling/blending/palette is independent.
- **Jolt physics** — `JoltJobsAdapter` (`plugins/jolt_jobs_adapter.h`)
  implements `JPH::JobSystemWithBarrier` over `jobs::run`, so Jolt spawns
  zero threads. `PhysicsSystem::Update`'s barrier wait executes queued jobs
  on the sim thread too; `Job::Execute`'s internal CAS makes double pickup
  safe. Requires Jolt built with `CPP_RTTI_ENABLED` (typeinfo for the
  out-of-line base class).

## Future Work
- ~~AsyncLoader onto the pool's IO channel (currently its own thread).~~
  **Already done, and this line was stale.** `AsyncLoader` owns no thread —
  `grep` for `std::thread` under `services/async_loader/` returns nothing; it
  schedules through `jobs::run("io.assetLoad", …)` (`loader.cpp:96`).
  What remains is the thing this line was really asking for: streaming work
  currently inherits the pool's `USER_INITIATED`, and there is no way to say
  "run this at a lower priority than the frame". That is a **job priority**
  concept in the facade, not a thread to move — noted while wiring thread QoS,
  which could not express it either.
- Cook pipeline parallel per-asset cooks.
- Renderer extraction/culling tasks.
- C API surface for kits (`engineJobs*`) once a use case appears.
- Per-task memory tags once the memory manager lands.

**2026-11-11.** `jobs::init` holds a `mem::SystemNewScope` around the
scheduler's start-up: enkiTS makes its workers with `std::thread`, whose
bookkeeping the system's C++ library frees at thread exit, so it must not come
from the engine's heap (`src/core/info.md`, `eng_shared_test`). enkiTS's own
arrays still come from `Tag::Jobs`.
