---
status: as-built
tier: hardened
verified: 2026-08-29
covers:
  - src/runtime/jobs/
tests:
  - tests/jobs_test.cpp
  - tests/stress_jobs.cpp
---
# engine::jobs — the engine-wide task scheduler

One worker pool for the whole engine, spawned once at `EngineRuntime::init`
(`jobs::init`), torn down in `shutdown()`. Thread creation is a boot event,
never a frame event — this subsystem exists so the frame loop makes no
threading syscalls (workers spin-then-park only when truly idle).

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

## Backend & the FTL swap contract
Current backend: **enkiTS** (`jobs_enkits.cpp` — the only TU in the engine
that includes enkiTS; built directly in the root CMakeLists, imgui-style,
because its own CMakeLists predates CMake 4 minimums). The facade (`jobs.h`)
is written so **FiberTaskingLib** could replace it with one sibling .cpp:

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
- AsyncLoader onto the pool's IO channel (currently its own thread).
- Cook pipeline parallel per-asset cooks.
- Renderer extraction/culling tasks.
- C API surface for kits (`engineJobs*`) once a use case appears.
- Per-task memory tags once the memory manager lands.
