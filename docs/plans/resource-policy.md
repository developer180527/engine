---
status: plan
covers:
  - src/runtime/
---
# Resource policy — how much of the machine to take

> **Status: plan, but no longer entirely unbuilt.** Thread QoS (§3.1) landed
> 2026-09-04 and the texture budget 2026-09-06; both are marked in place. Every
> measurement cited was taken on this tree; everything not marked as built is
> still a proposal.

## 0. The principle

> **The engine should size itself to the WORK, not to the machine.**

Not "let the developer pick a tier" — a tier is a knob nobody tunes correctly,
and a wrong tier is worse than none because it looks deliberate. The right number
of worker threads is a function of how much parallel work exists, which the
engine can measure. The developer's job is to set a **ceiling**, not to guess the
number.

The failure this exists to prevent is concrete: **a 2D puzzle game currently gets
eleven worker threads and an uncapped frame rate**, because those are the
machine's numbers rather than the game's. It costs battery, heat and fan noise to
produce nothing.

The opposite failure is just as real. A AAA title that wants every core and every
watt should be able to say so, and today it cannot say that either — it simply
gets whatever the defaults happen to be.

## 1. What exists today: one knob

`EngineConfig` (`runtime.h:42`) has **one** resource control:

```cpp
int meshBudgetMB = 0;   // 0 = unbounded
```

Everything else is a default nobody chose:

| resource | today | chosen? |
|---|---|---|
| worker threads | `jobs::init()` with no argument → enkiTS default, `cores - 1` | no |
| **thread QoS / priority** | ~~never set~~ **set, as of 2026-09-04** (§3.1) | **yes** |
| core affinity (P vs E, big.LITTLE) | none | no |
| frame cap | none | no |
| vsync | `BGFX_RESET_VSYNC`, hardcoded (`device.cpp:117`) | no |
| resolution scale | none | no |
| memory budgets | **all three reachable** — `meshBudgetMB`, `textureBudgetMB` (both evict) and `memBudgetMB[]` (soft, reports only) | **yes** |
| idle / background throttle | none | no |

The memory row deserves its own line. There were **two separate budget systems
and no production code set either**; one is now wired:

* `GpuResourceCache::setBudget(bytes)` — **WIRED 2026-09-06** as
  `EngineConfig::textureBudgetMB`, evicted on the same ~1 s tick as the mesh
  sweep. It turned out not to be plumbing: eviction against wrong reference
  counts drops textures still in use, and `unloadTexture` was bypassing the
  refcount entirely (BUG-0051). The ctor comment saying eviction "needs the
  reference counts to be complete first" was the real blocker, and it was
  correct.
* `mem::setBudget(Tag, bytes)` — **WIRED 2026-09-06** as
  `EngineConfig::memBudgetMB[]`, indexed by `mem::Tag`. This one *was* plumbing.
  **It is SOFT and that is a contract, not a shortfall**: the allocator warns
  once per tag and keeps going. A hard CPU cap means deciding what to do when a
  gameplay allocation cannot be served, and "crash the game to respect a number
  in a config file" is not an answer — so it is a tripwire for "this subsystem
  is bigger than I thought", not a limit. `mem_test` asserts the softness
  directly, because "the budget did nothing" and "the budget is soft" look
  identical from outside and only one is the design.

## 2. Three levels, and most projects use one

| level | who decides | example |
|---|---|---|
| **1. Profile** | the developer states intent | `Minimal` / `Balanced` / `Maximum` |
| **2. Override** | a developer who measured | `workers = 4`, `frameCap = 60` |
| **3. Adaptive** | the engine, at runtime | dynamic resolution when frame time slips |

Level 1 is what a 2D game sets and never revisits. Level 2 exists so that the
person who profiled is not forced back to a preset. Level 3 is the only one that
can respond to a machine the developer never tested on, and it is also the only
one that can make a game feel worse if it is wrong — so it should be opt-in and
bounded by level 2's ceilings.

**A profile is a set of defaults, not a mode.** Nothing in the engine may branch
on "are we in Minimal" — it reads the resolved numbers. Otherwise the profiles
become three code paths and two of them are untested.

## 3. CPU

### 3.1 Thread QoS — the measured one

The only item on this page with a number behind it, and it is large.

```
32 threads, 12 cores, 2 seconds of contention
  8  USER_INTERACTIVE threads   78,285 units each
  24 BACKGROUND       threads    5,407 units each
  → 14.5x per-thread advantage
```

Two findings from that pass, one of which corrected an assumption:

* **`main()` already gets `USER_INTERACTIVE`.** The main thread was never the
  problem — a hypothesis that it might be on E-cores was measured and refuted.
* **`pthread_create` produces a `DEFAULT` thread and does NOT inherit the
  creator's class.** So the enkiTS pool runs one class below the main thread, by
  omission rather than by decision.

**It does not matter on an idle developer machine** — 8 workers on 12 cores never
compete, and a direct measurement of that case showed all classes within noise.
It matters on a player's four-core laptop with a browser open, which is the
shipping condition rather than the desk condition.

**BUILT 2026-09-04** — `src/core/thread_qos.h`, `tests/thread_qos_test.cpp`.
The assignment as implemented, with two corrections this table originally got
wrong:

| thread | class | state |
|---|---|---|
| main / sim | `USER_INTERACTIVE` | already correct — the OS gives it, measured at 33 |
| enkiTS workers | `USER_INITIATED` | **done**, via `cfg.profilerCallbacks.threadStart` |
| ~~async loader~~ | — | **there is no such thread** (see below) |
| `CookService::cookLoop` | `UTILITY` | **done** — and it was never set, despite a comment claiming it was |
| cook worker (child process) | `UTILITY` | already correct (`engine_cook_worker.cpp:163`) |

`USER_INITIATED` rather than `USER_INTERACTIVE` for workers is deliberate: eleven
threads all claiming the top class is how a process starves its own main thread.

**Correction 1 — the async loader is not a thread.** This table listed it as one.
`AsyncLoader` owns no thread; it schedules through `jobs::run("io.assetLoad", …)`
on the shared pool (`async_loader/loader.cpp:96`). You cannot set a scheduling
class on a job, so that row was not implementable as written. Its work now
inherits the pool's `USER_INITIATED`, which is arguably too high for streaming —
but fixing that means a *priority* concept in the job system, not a QoS call, and
that is a different piece of work.

**Correction 2 — a comment asserted an invariant nobody had established.**
`engine_cook_worker.cpp:161` justifies re-demoting the spawned child with *"the
parent's cook threads are QoS-demoted"*. They were not. Before this change
nothing in the process set a class on any thread, so `CookService`'s loop
competed with the frame at `DEFAULT` while its own child process politely ran at
`UTILITY`. Now set at the top of `cookLoop()`, which makes the child's comment
true for the first time.

**A third fact, found by measurement:** a fresh `pthread` reports
`QOS_CLASS_DEFAULT` (21) — a real, distinct value, one step below the main
thread's `USER_INTERACTIVE` (33), not "unspecified". That is what makes the
non-inheritance a demotion rather than an absence, and `Class::Unclassified`
exists in the API so a test can tell the two apart.

Windows: `SetThreadPriority`, wired. `AvSetMmThreadCharacteristics("Games")` is
deliberately *not* called — it requires a matching revert and therefore an owner
with a lifetime, which a fire-and-forget setter does not have.
Linux: per-thread `setpriority` with `SYS_gettid` (passing `0` would nice the
whole process and demote main with it). Raising priority needs privileges we
will not have, so `Interactive` is a no-op there rather than a failure.

**What the test can and cannot prove.** `thread_qos_test` asserts the pool's
workers land on `Initiated`, that the calling thread is *not* demoted with them,
and that an external kit/provider thread keeps the class its owner chose. It
does not assert that QoS makes anything faster — that is a property of the OS,
measured once above, not of this code. Strong readback exists only on Apple, so
the test gates on `classIsObservable()` rather than a platform macro.

### 3.2 Worker count

`cores - 1` is right for a title that saturates them and wasteful for one that
does not. Sizing to the work means the ceiling is the developer's and the
occupancy is the engine's.

Note the interaction with `kExternalThreadSlots = 8` — enkiTS already reserves
slots for threads the engine does not own (kits, provider decode workers). The
real thread count is already above the core count on a small machine, which is
exactly when §3.1's numbers start applying.

### 3.3 Frame pacing

Vsync is hardcoded to on. A frame cap that is *lower* than the display rate is a
legitimate and currently impossible request: a 2D game at 60 on a 240 Hz monitor
should not render 240 times.

## 4. GPU

**Resolution scale is the largest single lever and the cheapest to add.**
Rendering at 67% linear scale is under half the pixels, and §3.1's fragment/vertex
ratio is why that dominates: the fragment shader runs per covered pixel, the
vertex shader per vertex.

Then: frame cap, present mode (vsync / immediate / mailbox), quality tiers, and
upscaling — which is its own section because it is not a knob.

## 5. Upscaling, and what it demands before it can exist

For the backends `rhi-design.md` axiom 6 selects:

| backend | upscaler | why |
|---|---|---|
| Metal 4 | **MetalFX** (`MTLFXTemporalScaler`) | Apple's own, on Apple Silicon |
| Vulkan | **FSR** | open source, cross-vendor |

DLSS is NVIDIA-only and XeSS Intel-only; neither serves two backends, and adding
a third vendor SDK to reach one is the tax `provider-abi.md` §5 describes.

**Upscaling is not a feature that gets bolted on later.** Every temporal upscaler
requires the same four things from the renderer:

1. render at a lower resolution
2. **per-pixel motion vectors**
3. depth
4. a **jittered projection matrix** — sub-pixel offsets per frame, TAA-style

The renderer today has neither 2 nor 4, and `renderer-vs-production.md` records
`post: none`. So "add DLSS later" is really "add a velocity pass, jitter the
camera, and build a post chain" — months, not a checkbox, and it constrains what
`renderer-program.md` P4's render graph must be able to express.

**Half of it already exists, for an unrelated reason.** `components/prev_transform.h`
holds last sim tick's transform, written at the start of every fixed step so the
renderer can interpolate. That is the CPU-side input a velocity pass needs. The
renderer would still have to emit per-pixel velocity from it, but the data is
there and nobody put it there for this.

## 6. The rule that keeps this honest

> **A resource policy set by guesswork is worse than no policy**, because it
> looks like a decision.

The engine already has the instruments: `rdiag::SubmitStats` counts draws and
material binds on our side of the API, `MemoryChannel` reports per-tag bytes each
frame, `mem::mapEventCount()` is the syscall metric, and the profiler gives
per-scope timings. Nothing here should ship a default that was not measured
against them.

This repo has the precedent both ways: NEON was declined on a measurement showing
0.4% of a phase, and the residency sweep was fixed after measuring 0.463 ms at
50 000 entities. The QoS numbers above exist because the hypothesis was tested —
and half of it turned out to be wrong.

## 7. Order of work

| # | Work | Why here |
|---|---|---|
| ~~**1**~~ | ~~**Thread QoS**~~ — **DONE 2026-09-04.** `src/core/thread_qos.h`, wired into the enkiTS pool and `CookService`; `thread_qos_test` in the unit lane, mutation-checked | measured, and it needed no policy design. Found two doc errors and one false comment on the way |
| **2** | Frame cap + vsync as configuration rather than a hardcoded reset flag | small, and the 2D case is unserved today |
| **3** | Worker-count ceiling in `EngineConfig`, defaulting to today's behaviour | no behaviour change until someone sets it |
| **4** | ~~Wire **both** budget mechanisms to config~~ — **DONE 2026-09-06.** `textureBudgetMB` evicts on the mesh sweep's tick; `memBudgetMB[]` sets soft per-tag ceilings | the texture half was NOT plumbing — it was blocked on correct refcounts (BUG-0051), and then on `unloadMesh` never releasing the references `loadMesh` took, without which eviction reclaimed nothing on the path a game uses. The `mem::` half really was plumbing |
| **5** | Resolution scale | the largest GPU lever; needs a render target the renderer does not currently size independently |
| **6** | Upscaling | blocked on §5's motion vectors and jitter — a renderer-programme dependency, not a policy one |

Items 2–4 need no new architecture and no renderer work. Item 1 is done.

## 8. Open questions

1. **Does a profile ship in `project.json`, or is it code?** Data means a player
   could be given the switch; code means it cannot be got wrong. Probably data
   with a code override, but undecided.
2. **Does the engine ever lower a ceiling it was given?** Level 3 implies yes;
   a developer who set `workers = 8` may disagree. Recommend: adaptive moves
   *within* the ceiling and never above it.
3. **Who owns throttling while suspended?** The embedder knows the app is
   backgrounded (`platform-embedder.md` §4); the policy knows what "reduced"
   means. They have to meet, and neither document currently says where.
4. **Is there a headless/server profile?** A dedicated server wants zero GPU, all
   CPU, and no frame pacing at all — which is a fourth shape, not a point on the
   Minimal→Maximum line.
