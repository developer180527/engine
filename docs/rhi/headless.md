---
status: decided
covers:
  - src/runtime/
---
# Running without a GPU: what it costs and what it actually buys

> **The question this answers:** a dedicated server must not wait for anything —
> take input, simulate, ship state. So what technique do shipping engines use to
> drive the cost of a "null renderer" to zero?
>
> **The answer:** none, because there is nothing to drive to zero. The dispatch
> cost is 0.000037% of a server tick. What shipping engines actually eliminate is
> four to eight orders of magnitude larger, and it is not dispatch.

## 1. The arithmetic that closes the question

`EngineRuntime` reaches **17 distinct `Renderer` methods**, plus `submitDraw`
from `engine_api.cpp` — 18 in total (counted 2026-09-05). Only a handful are
called on any given tick, so treating all 18 as per-tick is a deliberate
over-estimate. At the 0.68 ns measured in [`swappability.md`](swappability.md)
§2 (`tests/perf/seam_cost_bench.cpp`), routing every one of them through a
virtual `IRenderer` costs:

```
  18 calls x 0.68 ns  (over-estimate)  =        12 ns per tick
  a 30 Hz server tick                  = 33,333,333 ns
                                       -> 0.000037% of the tick
  a 128 Hz tick                        ->  0.00016% of the tick

  calls/tick needed to reach 1% of a 30 Hz tick:  ~490,000
```

**There is no technique to find, because there is nothing to eliminate.** Any
design chosen for this number is being chosen on noise.

One call deserves naming, because it is the exception to "frame-level":
`Renderer::submitDraw` is the immediate-mode kit path, called **per draw**, from
the job pool, already taking a lock. It is capped at 1 024 submissions per frame,
so worst case is ~0.7 µs/frame. Still noise — but it is the one place where
[`swappability.md`](swappability.md) §1's granularity rule applies, so it should
be a deliberate decision rather than a side effect.

## 2. What a server is actually not waiting for

The instinct behind the question is right. The targets are just much bigger.

| What is eliminated | Scale | vs. the 12 ns |
|---|---|---|
| **`present()` / vsync** — a client frame *deliberately blocks* to align with the display | up to 16.7 ms at 60 Hz | **~1,400,000x** |
| **Swapchain / drawable acquisition** — unbounded, not merely slow | measured **~1 second** in this repo | **~82,000,000x** |
| **GPU asset loading** — textures, shaders, meshes never leave disk | GB of IO, RAM, VRAM, startup | not comparable; the largest by volume |
| **Render-only subsystems** — particles, VFX, post, cosmetic animation | whole subsystems | — |
| Driver, swapchain, display server, compositor | whole dependencies | — |

The drawable stall is measured **in this tree**, at
`src/render/renderer/device.cpp:72`:

> *"Two of three multithreaded runs stall for ~1 SECOND … 'blocked waiting for
> next drawable' … Metal drawable acquisition starving."*

That is the waiting. A client is at the mercy of a display pipeline it does not
control; a server has no display pipeline at all. **The win is the absence of a
GPU, not the cheapness of not calling one.**

## 3. What Unreal actually does

Verified against Epic's own documentation on 2026-09-05 — rung 3 by
[`workflow.md`](workflow.md) §1. This section is the reason the recommendation
below changed.

**Unreal ships TWO mechanisms, and they are not alternatives.**

### 3.1 `FNullDynamicRHI` — for running headless, not for servers

> *"A null implementation of the dynamically bound RHI."*
> — module `NullDrv`, `/Engine/Source/Runtime/NullDrv/Public/NullRHI.h`,
> hierarchy `FDynamicRHI` → `FDynamicRHIPSOFallback` → `FNullDynamicRHI`

Selected with `-nullrhi`, whose official description is decisive about its
purpose:

> *"Use null rendering hardware interface to run UE headless."*

**Note where it sits: the RHI layer, the BOTTOM.** Under `-nullrhi` the entire
renderer still runs — culling, draw building, RHI command lists — and simply
terminates in a backend that does nothing. It is a null object at the driver
seam, not a null *renderer*.

Its purpose is build machines, CI and automation. **It is not how Epic ships
dedicated servers.**

### 3.2 `TargetType.Server` — for servers

A `.Target.cs` sets `Type = TargetType.Server`:

> *"Same as Game, but does not include any client code. Useful for dedicated
> servers in networked games."*

This is **compile-time exclusion**: the client code is not in the binary.
`TargetRules` also exposes `bWithServerCode` — *"Compile server-only code."*

### 3.3 Cook-time exclusion — the biggest win, and it is a COOKER feature

The mechanism is per-class and per-object, not per-asset-type:

* **`UObject::NeedsLoadForServer()`** (and `NeedsLoadForClient()`) — return
  false and the object is **discarded on servers**.
* **`ClassesExcludedOnDedicatedServer` / `ClassesExcludedOnDedicatedClient`** in
  the cooker settings feed those predicates.
* **`UWorld::SpawnActor` checks the same predicates**, so the exclusion holds at
  runtime as well as at cook time.

This is the pattern most worth copying, and it belongs in
`src/assets/cookers/`, not in the renderer.

### 3.4 What could not be verified

Stated because an unsourced claim that looks sourced is worse than none:

* **`UE_SERVER` and `WITH_SERVER_CODE`** were asserted as macro names in an
  earlier draft and **could not be verified**. The UBT *property* is
  `bWithServerCode`; the preprocessor spelling is not in any reachable doc.
  Retracted.
* One fetched page rendered the Server target's description as *"does not
  include any **server** code"* — self-contradictory, and a mis-attribution of
  the Client row. Corrected against a second source. Recorded because it is the
  failure mode a single-source citation has.

## 4. Where this engine already is

**A null renderer already exists here. It is spelled `if (!m_headless)`, in nine
places across three files** — `runtime.cpp`, `runtime_boot.cpp` and
`runtime_frame.cpp` (counted 2026-09-05). In spirit that is `-nullrhi`: the same
binary, skipping presentation.

**And the scattered form has already shipped a defect.** From
`src/runtime/docs/issues.md` (2026-08-10):

> *"A dedicated server leaked every draw submission, forever.* `Renderer::frame()`
> cleared the external submission list, and the runtime calls `frame()` only
> `if (!m_headless)` — while `engineDrawSubmitBindRenderer(&m_renderer)` is
> unconditional. So on a server the list filled and nothing ever drew it:
> **~480 KB/s** at 100 submissions a tick."

One guard present, an adjacent one missing, silently, for as long as it existed.
The fix split out `endFrame()` — patching one hole in a design that has nine. That
is the argument for the null object, and it is a correctness argument, not a
performance one.

## 5. The recommendation: A then B, and they are not alternatives

The earlier framing in [`phases.md`](phases.md) presented these as a choice.
Unreal ships both, for different jobs, and so should this engine — in this order.

| # | Step | Goal | UE's equivalent | Status |
|---|---|---|---|---|
| **A** ✅ | **`IRenderer` + `NullRenderer`**, pure virtual so a missing override is a compile error | the same binary runs with no GPU: tests, cook worker, `engine_host`, CI | `FNullDynamicRHI` / `-nullrhi` | **landed 2026-09-05**, `tests/null_renderer_test.cpp` |
| **B** | **A build target that excludes the render TUs and bgfx** | a lean dedicated server binary | `TargetType.Server` | not started; **this is what G1c's exit criterion actually asks for** |
| **C** | **Cook-time class exclusion** | the assets never exist server-side | `NeedsLoadForServer` | not started; **largest win, and a cooker feature** |

**A does not deliver B.** Under A the renderer still links, still runs, and still
builds draw lists into nothing — which is exactly what `-nullrhi` is, and exactly
what Epic does *not* ship servers as. Anyone expecting A to produce a lean server
will be disappointed for a structural reason, not a tuning one.

> **A landed 2026-09-05, and the caveat above is now a measured fact rather than
> a prediction: `engine_runtime` still links bgfx.** `runtime.h` holds a
> `std::unique_ptr<IRenderer>` and names no concrete renderer — `runtime_boot.cpp`
> is the only runtime TU that knows `Renderer` and `NullRenderer` exist — but the
> binary contains both. B is untouched.
>
> What A *did* deliver: **four of the nine `if (!m_headless)` guards are gone**
> (`shutdown`, the `frame`/`endFrame` branch, `resize`, `renderScene`), and with
> them the shape of the 480 KB/s leak. The other five stay on purpose and are not
> renderer guards — one is the platform's minimize-wait, one is `tick()`'s "did we
> render" return, one sets the flag, and two skip real work (source-importer
> registration, primitive-mesh building). Deleting those would make a server do
> work for nothing, which is the failure §1 warns against.

**A is still worth doing first**, and on its own merits: it deletes nine scattered
guards with a proven failure mode, it makes "what does headless do" answerable in
one file instead of by grep, and it is what the tests and the cook worker
actually need. Its value does not depend on a server existing.

### Why axiom 5's hazard does not apply

[`design-axioms.md`](design-axioms.md) axiom 5 warns that a second
implementation becomes the untested one. That is a warning about an
implementation producing **different answers** — the CPU cull can silently
diverge from the compute cull.

**A do-nothing implementation has no answers to diverge.** With pure virtuals,
adding a method to `IRenderer` is a *compile error* until `NullRenderer`
implements it: rot is compiler-caught rather than review-caught.

The exception is real and small — three methods whose null version must return
something *meaningful* rather than nothing: `homogeneousDepth()`,
`sceneW()`/`sceneH()`, and `submittedDrawCount()`. Those can be wrong, and
`homogeneousDepth()` is the one to check first, because it feeds
`m_cameraFinder.find()` on a path the headless early-return does not cover.

## 6. The rule this leaves behind

> **Headless performance comes from not having a GPU, not from not calling one.**

Any future proposal justified by "it makes the headless path faster" should be
checked against §1 first. If the mechanism it removes is measured in nanoseconds
per frame, it is not a performance change and should be argued for on
correctness — which is a perfectly good argument, and the one that carries A.
