# Performance

## Profiler (`core/profiler.h`)

An **extensible, instrumenting** profiler. A `Profiler` hub owns the frame
lifecycle and a registry of *channels*; each kind of profiler (timing, memory,
GPU...) is an `IProfilerChannel`. The **timer** is the first channel; add more
by implementing the interface and `Profiler::get().addChannel(...)`.

It is an *instrumenting* profiler (named scopes you place), **complementary**
to a *sampling* profiler (Instruments / perf / VTune). This tells you which
engine **phase** is hot; a sampler tells you which **instruction**. Don't
conflate them.

### Using it

```cpp
#include "core/profiler.h"

void mySystem() {
    ENGINE_PROFILE_SCOPE("MySystem");   // times this block
    ...
    { ENGINE_PROFILE_SCOPE("SubStep"); ... }  // nests automatically
}
```

- `ENGINE_PROFILE_SCOPE("name")` — RAII scope; the name must be a string
  literal (its address is the sample id — no hashing, no allocation).
- `ENGINE_PROFILE_FUNC()` — scope named `__func__`.
- Toggle with `EngineConfig::enableProfiler` (default: on in debug, compiled
  out entirely in shipping release via `ENGINE_PROFILE=0`).

The runtime instruments boot (`boot.platform/renderer/systems/...`, dumped as
one line at init) and each frame's phases (`AsyncDrain`, `Animation`,
`ECS.progress`, `Render`, `Sim.update/physics/post`, `bgfx.frame`).

### Cost & the discipline rule

- Compiled out in release → **zero** cost in shipped games.
- Compiled in but disabled → one relaxed atomic load (~1 ns), dormant.
- Enabled → a thread-local buffer write (no locks, no allocation after warmup).

**The rule that keeps it honest:** scope at **phase granularity** (physics,
render, animation), **never** inside a hot per-iteration loop. A scope costs
tens of ns; place one in a 100k-entity loop and the instrumentation perturbs
what it measures (observer effect). Time the whole loop as one scope and let a
sampling profiler go deeper.

### Threading

Each thread records into its own buffer (lock-free hot path) — so the profiler
works inside Jolt's threads and the future job pool. The frame boundary
(`beginFrame`/`endFrame`) is the sync point where the main thread collects all
buffers; it assumes worker jobs are joined by then (the threading contract).

### Verification

- `profiler_test` (engine_core, headless): scope-timing/nesting/depth/dormant
  correctness.
- `profiler_frames` (engine_runtime, headless): the real frame-loop path
  through a headless runtime — boots, ticks frames, dumps the phase breakdown.

### Hardening (timer)

- **Fixed-capacity buffers** (`kMaxSamplesPerThread`): no realloc after warmup;
  overflow drops and warns once (`lastFrameDropped()`), never grows unbounded.
- **Parent IDs**: each sample stores its parent's index (pre-order emission),
  not only a depth — exact tree/flamegraph reconstruction. Depth kept for quick
  indentation. Indices are rebased into the merged buffer at collect time.
- **Clock seam** (`prof::Clock`): portable `std::steady_clock` (the best
  monotonic source per platform — mach on Apple, QPC on Windows, MONOTONIC on
  Linux); isolated so it can be swapped (rdtsc, a fake clock for deterministic
  replay) without touching callers.
- **Cache-line aligned recorders** (`alignas(64)`): false-sharing insurance —
  the correct use of cache-line padding (between-thread hot data), as opposed
  to the wrong use on densely-iterated components.

## Memory channel (`runtime/mem_channel.h`) — measure, don't replace

The framework's first extra channel. It MEASURES per-frame allocations; it
never replaces an allocator (so ASan/`leaks`/Instruments keep working):

- **C++ new/delete** — `core/mem_counters.h`: a process-wide counting
  `operator new`/`delete` override that COUNTS and FORWARDS to malloc/free.
  Gated by `ENGINE_MEM_COUNT` (default on in debug, compiled out in release).
- **flecs (C malloc)** — flecs's built-in `ecs_os_api_*_count` globals.

Why measure-first matters: a steady headless frame already reports
`flecs alloc:0, C++ new:0` — flecs reuses its tables, so it allocates nothing
per frame. That is the honest answer to "is malloc my stutter source?" before
any pooling is considered.

The disciplined path if a library ever DOES show per-frame churn: hook that one
library through its OWN allocator API (flecs `ecs_os_set_api`, Jolt
`JPH::Allocate`, Lua `lua_newstate`, bgfx `bx::AllocatorI`) — never a global
`--wrap`/operator-new REPLACEMENT (broken on macOS's ld; misses all C
libraries since they use malloc not `new`; cross-boundary free corruption;
destroys the debugging tools). And own transient allocation with a frame arena
(our code, explicit), not by intercepting libraries.

### Roadmap

- **P2** ✅ — editor overlay panel (`editor/panels/profiler_panel.h`,
  menubar → Profiler → Frame Profiler): rolling frame-time graph, per-phase
  table (parent-ID tree), flamegraph, and a Memory section (C++/flecs alloc
  counts + frame-arena usage/high-water). Walks the channel registry.
- **P3** — Chrome-trace export (Perfetto/`chrome://tracing`, uses the parent
  IDs) + a GPU channel reading `bgfx::Stats`. The channel registry is built
  for exactly this.

## Threading model (current + intended)

The engine is **multithreaded but siloed** today: Jolt runs its own
`hardware_concurrency-1` pool, CookService runs a background pool, asset decode
is off-thread. The frame *orchestration* is single-threaded. flecs ships a
parallel scheduler (`set_threads` + `multi_threaded()` systems) we have not yet
switched on.

Intended direction (designed, not built): **one shared job system** as the
spine — flecs and (behind a benchmark) Jolt migrate onto it; `engine::jobs`
exposes `parallel_for` for non-ECS work. Rules when that lands:

- **Main thread only:** bgfx/GPU submission + uploads, ScriptHost / C API,
  platform calls, ECS structural changes (outside `defer`).
- **Parallel-safe:** `multi_threaded()` flecs systems (flecs batches entities),
  `jobs::parallel_for` for the rest, disjoint component writes only.
- **Never** spawn raw `std::thread` for per-frame work — use the shared pool.
- **Never** block a job on main-thread-only work (GPU, ScriptHost).
- **Lua stays on the main thread, permanently** — the VM is not thread-safe.
  Scripts orchestrate; C++/C# systems parallelize.

## Rejected (with reasons, so they're not re-proposed)

- **Fiber scheduler** — migrating fibers across threads breaks `std::mutex`
  ownership, the thread-bound Lua VM, and bgfx single-threaded mode;
  `makecontext` is deprecated on macOS; the kernel-context-switch saving is
  unmeasurable at our scale. Maximum risk, no measurable benefit.
- **Zero global runtime malloc** — incompatible with flecs/Jolt/Lua/Assimp/bgfx,
  which all allocate. We adopt the *useful subset* (a frame arena for transient
  allocations) without the totalizing goal.
- **`alignas(64)` on every component** — backwards for ECS: components are
  iterated densely in columns; 64-byte padding gives one component per cache
  line and destroys locality. Cache-line padding is for false-sharing between
  threads (atomics), not for iteration data. Field ordering large→small is the
  free win.
