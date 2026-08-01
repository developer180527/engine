---
status: as-built
tier: working
verified: 2026-08-01
covers:
  - src/runtime/jobs/
tests:
  - tests/jobs_test.cpp
---
# engine::jobs — the engine-wide task scheduler

One worker pool for the whole engine, spawned once at `EngineRuntime::init`
(`jobs::init`), torn down in `shutdown()`. Thread creation is a boot event,
never a frame event — this subsystem exists so the frame loop makes no
threading syscalls (workers spin-then-park only when truly idle).

## Backend & the FTL swap contract
Current backend: **enkiTS** (`jobs_enkits.cpp` — the only TU in the engine
that includes enkiTS; built directly in the root CMakeLists, imgui-style,
because its own CMakeLists predates CMake 4 minimums). The facade (`jobs.h`)
is written so **FiberTaskingLib** could replace it with one sibling .cpp:

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
