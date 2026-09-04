---
status: plan
covers:
  - src/runtime/
---
# Resource policy — how much of the machine to take

> **Status: plan. Nothing here is built** except the one knob §1 records. Every
> measurement cited was taken on this tree; every design below is a proposal.

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
| **thread QoS / priority** | **never set** | no |
| core affinity (P vs E, big.LITTLE) | none | no |
| frame cap | none | no |
| vsync | `BGFX_RESET_VSYNC`, hardcoded (`device.cpp:117`) | no |
| resolution scale | none | no |
| memory budgets | **two** mechanisms, neither reachable from config | no |
| idle / background throttle | none | no |

The memory row deserves its own line, because there are **two separate budget
systems and no production code sets either**:

* `mem::setBudget(Tag, bytes)` — a per-tag ceiling on the tagged heaps that the
  allocator warns against. Called from `tests/mem_test.cpp` and nowhere else.
* `GpuResourceCache::setBudget(bytes)` (`render/gpu_resource_cache.h:84`) — the
  GPU-side eviction budget. Called from `tests/gpu_cache_test.cpp` and nowhere
  else.

Both are the shape of the right answer, already written, already tested, and
wired to nothing a developer can reach. That is a cheaper starting point than it
looks: item 4 in §7 is plumbing, not design.

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

Recommended assignment:

| thread | class | why |
|---|---|---|
| main / sim | `USER_INTERACTIVE` | already correct |
| enkiTS workers | `USER_INITIATED` | on the frame's critical path — but see below |
| async loader | `UTILITY` | it exists to yield |
| cook worker | `UTILITY` | already correct (`engine_cook_worker.cpp:163`) |

`USER_INITIATED` rather than `USER_INTERACTIVE` for workers is deliberate: eleven
threads all claiming the top class is how a process starves its own main thread.

Windows: `SetThreadPriority` plus `AvSetMmThreadCharacteristics("Games")`.
Linux: nice values, or `SCHED_FIFO` where permitted.

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
| **1** | **Thread QoS** on the four thread classes in §3.1 | measured, ~5 lines, and it is the only item with a number. Needs no policy design |
| **2** | Frame cap + vsync as configuration rather than a hardcoded reset flag | small, and the 2D case is unserved today |
| **3** | Worker-count ceiling in `EngineConfig`, defaulting to today's behaviour | no behaviour change until someone sets it |
| **4** | Wire **both** budget mechanisms to config — `mem::setBudget` and `GpuResourceCache::setBudget`, each currently reachable only from its own test | plumbing, not design |
| **5** | Resolution scale | the largest GPU lever; needs a render target the renderer does not currently size independently |
| **6** | Upscaling | blocked on §5's motion vectors and jitter — a renderer-programme dependency, not a policy one |

Items 1–4 need no new architecture and no renderer work. Item 1 is worth doing
this week and independently of everything else on this page.

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
