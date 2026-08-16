---
status: plan
covers:
  - include/engine/
---
# Replaceable subsystems: the provider ABI contract

> **Status: one header written, nothing live.** `EngineAudioProviderV1` exists and
> has a conformance suite; no host loads it yet, and miniaudio is still wired
> directly into a C++ plugin. This document is the contract every provider ABI
> follows, the shape each subsystem's seam has to take, and the two rules that
> stop a seam from rotting. Written to be argued with before physics copies audio.

## 0. The finding that reorders everything

**There are three different seams in this engine and they are being treated as
one.** Nothing else in this document matters as much.

| | What it is | ABI | Coupled to | Who writes one |
|---|---|---|---|---|
| **A. Inbound extension** | a module calls the ENGINE | C, frozen, versioned (`EngineApiTableV1`) | nothing — fixed-width types only | anyone, any language |
| **B. Outbound provider** | the engine calls a REPLACEABLE subsystem | C, frozen, versioned (`EngineAudioProviderV1`) | nothing | anyone, any language |
| **C. Internal plugin** | `IEnginePlugin`, `IPhysicsService`, `IAudioService` | **C++ vtables** | **flecs, RuntimeContext, our STL, our compiler** | us |

Seam A is done and hardened. Seam B has exactly one header. **Physics and audio
both live in seam C today** — `AudioPlugin` is `IEnginePlugin` + `IAudioService`
holding an `ma_engine`; `JoltPlugin` is the same shape with `onPhysicsStep(flecs::world&, float)`.

That matters because seam C is not a stable seam and cannot be made into one. A
studio replacing physics through it must compile with our compiler, our C++
standard library version, our flecs version, and against our internal headers — and
must not let an exception cross. That is the definition of the lock-in you want to
avoid, and it is currently the only door available.

So the goal is not "add ABIs". It is **move each replaceable subsystem from seam C
to seam B, and stop advertising seam C as an extension point.** Seam C keeps
earning its place for first-party code built in this repo, where a vtable is
cheaper and more expressive than a C table. It is an internal convenience, not a
contract.

## 1. The rule that makes a seam free

An ABI's cost is **calls per second × cost per call**. A cross-dylib indirect call
is 1–2 ns and cannot be inlined. You do not make that cheaper; you arrange to make
far fewer of them.

So the design question for every subsystem is: **where is the low-frequency waist?**

Audio's answer is the reason its ABI is essentially free, and it is worth stating
as the template:

> The provider owns the device, the real-time thread and the mixer. At 48 kHz with
> a 128-frame buffer the mixer runs 375 times a second and crosses the ABI **zero**
> times. What crosses is CONTROL — one batched call per frame, a few hundred calls
> a second, against a 16 600 µs budget.

Contrast the numbers honestly:

| Seam placed at… | crossings/sec | overhead |
|---|---|---|
| audio control (1–3 calls/frame @ 120 fps) | ~360 | **~1 µs/sec — unmeasurable** |
| physics bulk sync (2 arrays/step @ 60 Hz) | ~120 | unmeasurable |
| physics **per body** (5 000 bodies, read+write) | 600 000 | ~1 ms/sec — 0.02 ms/frame, tolerable |
| renderer **per draw** (100 000 draws @ 120 fps) | 12 000 000 | **~20 ms/sec, ~0.2 ms/frame just in call overhead** — and that ignores the lost inlining and the marshalling |

Two conclusions follow, and the second is the one that will hurt:

1. **Per-object ABI calls are fine in the thousands and wrong in the hundreds of
   thousands.** Physics can afford a sloppier boundary than the renderer can.
2. **The renderer has no low-frequency waist at the draw level.** It therefore
   *cannot* use audio's shape, and any attempt to give it a `submitDraw`-per-object
   provider ABI will nuke exactly the performance you are trying to protect. §4.3.

And the cost that is not call overhead, which matters more: **data layout.** An
opaque-handle accessor forces the provider to give up its own SoA layout, or to
copy. Bulk arrays with a stride let it `memcpy` a shape it chose. That is why every
rule below is about batching, not about speed.

## 2. The contract — eight rules, each a refusal

Derived from `EngineAudioProviderV1`, generalised. A new provider ABI that breaks
any of these is not a variation, it is a mistake we have already made.

1. **The provider owns its resource, its threads and its schedule.** Never "the
   engine calls you to fill a buffer" — that puts the ABI *inside* the hot loop and
   makes allocation, locking and unwinding rules a joint problem across a boundary.
2. **Every bulk call takes `(ptr, count, stride)`.** The stride is not optional
   politeness: without it, adding a field to a row silently misaligns every read in
   every provider already compiled, because the provider indexes with *its own*
   struct definition. Vulkan and D3D12 both pass strides for this reason. Providers
   must walk by the given stride, never by `sizeof`, and ignore trailing bytes they
   do not recognise.
3. **Send only what CHANGED.** Audio sends rows for emitters that moved; physics
   returns transforms for bodies that are awake. A 5 000-body scene with 200 awake
   costs 200 rows. This single rule is worth more than every micro-optimisation in
   the ABI put together.
4. **Handles, never pointers.** Opaque `uint64_t`, zero always means "none", so a
   zero-initialised struct is valid. The provider's internals stay its own and a
   stale handle is a rejected lookup rather than a crash.
5. **Data, not callbacks**, for anything the provider consumes on its own schedule.
   Acoustic geometry and collision meshes are handed over as arrays the provider
   copies during the call. A callback would invert the threading (its workers, not
   ours), run at the wrong rate, and couple the two lifetimes.
6. **Fixed-width types only.** No `bool`, no `size_t`, no plain `enum`, no C++
   types. `structSize` on every struct that is passed by pointer; append-only;
   layout frozen by `static_assert` on both sizes and offsets. No exception or
   panic may escape — a Rust provider uses `panic = "abort"` or `catch_unwind` at
   every entry.
7. **"Unsupported" is a normal answer.** `E_UNSUPPORTED` from a provider that does
   no acoustic propagation, no soft bodies, no whatever, must leave the engine
   working with that feature simply absent. This is what makes a *minimal* provider
   legal, and a minimal provider is what a studio actually writes first.
8. **One escape hatch: `setParam(objectId, nameHash, value)`.** The engine never
   interprets `nameHash`; it forwards. This is what stops the interface growing a
   function per vendor feature — a Wwise RTPC, an HRTF dataset toggle, a solver
   iteration count. **The test before adding any function: could an FMOD or Wwise
   (or PhysX, or Havok) adapter implement it?** If not, it describes our
   implementation rather than the domain.

## 3. The two rules that stop a seam from rotting

This repo already has the cautionary example. `IRenderPipeline` is a real swap
point with **one implementation**, and `RenderContext` hands pipelines
`bgfx::TextureHandle` and `bgfx::ViewId` — so a second implementation cannot exist
without leaking bgfx into it. The seam was never exercised, so it decayed into
decoration. An ABI nobody crosses is a comment.

**Rule 1: the default implementation must go through the ABI.** miniaudio does not
get to be a C++ plugin holding `ma_engine` while the provider ABI sits beside it
unused. It becomes `engine_audio_miniaudio`, a provider like any other, loaded
through `ENGINE_AUDIO_PROVIDER_ENTRY`. If the first-party implementation does not
need the seam, the seam is wrong, and we will find that out on day one instead of
when a studio reports it.

**Rule 2: every provider ABI ships an executable specification.**
`tests/audio_conformance` is a Rust crate that hand-transcribes the C header,
implements a reference provider, and holds any provider to the contract — including
hostile input, stale handles, and a deliberately wider row stride. Rust matters:
it proves the ABI is implementable **from the C declarations alone**, which is the
actual claim being made when we tell a studio they can write a provider in any
language. A C++-only conformance test would prove nothing of the sort.

Both rules have a corollary that is also a measurement: **A/B the default provider
against itself.** Build it statically linked and direct, then through the ABI as a
dylib, run the same scene, compare. That is the only honest answer to "does the seam
cost anything", and this repo already decides things that way (SDL3 vs GLFW,
scalar vs NEON, one thread vs two). Until that number exists, "without nuking
performance" is an aspiration.

## 4. The shape of each seam

### 4.1 Audio — done on paper, and the template

`EngineAudioProviderV1`. Provider owns device/thread/mixer; control crosses at
frame rate; emitters batch with a stride; geometry is data; `setParam` is the
escape hatch. Remaining work is entirely on our side: a host loader, and moving
miniaudio behind the ABI (rule 1).

### 4.2 Physics — next, and the data flow is the whole design

Physics differs from audio in one way that drives everything: **the high-volume
traffic is bidirectional.** The engine writes kinematic poses, forces and
velocities; it reads back transforms and contacts. Both are per-body, both every
step. The waist is therefore two bulk arrays per step, not a call per body:

```c
/* ── the per-step waist ───────────────────────────────────────────────── */
void     (*writeBodies) (void* self, const EngineBodyWrite* w,
                         uint32_t count, uint32_t stride);
void     (*step)        (void* self, float dt, uint32_t substeps);
/* Returns only bodies that MOVED — the provider owns the active set, so this is
 * 200 rows in a 5 000-body scene, not 5 000. Rule 3, and it is the difference
 * between a seam that costs nothing and one that costs a millisecond. */
uint32_t (*readBodies)  (void* self, EngineBodyRead* out,
                         uint32_t max, uint32_t stride);
uint32_t (*readContacts)(void* self, EngineContact* out,
                         uint32_t max, uint32_t stride);

/* ── queries: BATCHED, because an FPS fires many per frame ───────────── */
/* AI line-of-sight, bullets, footstep probes. Per-call would be correct for
 * latency and wrong for volume, so the array form is the only form. */
uint32_t (*raycast)(void* self, const EngineRay* rays, uint32_t rayCount,
                    EngineRayHit* out, uint32_t stride);
```

Two things to get right that audio did not have to:

- **The character controller must not be in v1's core.** It is a gameplay opinion
  (step height, slope limit, skin width) that every physics engine models
  differently, and `IPhysicsService` already carries it as a no-op-defaulted
  virtual. Either it is a separate optional group with its own version, or it moves
  above the ABI and is built on raycasts and shape sweeps. Baking one engine's
  controller semantics into the ABI is precisely the "opinionated static
  philosophy" being avoided.
- **Determinism is a capability, not a promise.** Lockstep multiplayer needs it;
  most providers cannot give it across platforms. It belongs in a caps bitfield the
  engine queries, not in the contract.

### 4.3 Renderer — and why it must NOT copy audio

Per the table in §1: 100 000 draws through a per-draw ABI is ~0.2 ms/frame in call
overhead alone, before marshalling and lost inlining. **There is no waist at the
draw level.** So the seam has to sit at a coarser granularity, and there are only
three honest options:

1. **Pass-level** (what `IRenderPipeline` is, in C): the engine still extracts and
   culls; the provider implements passes. Cheapest to build, least freedom — the
   provider inherits our extraction, our culling, our `RenderItem`.
2. **Scene-handoff** (recommended): once per frame the engine hands over a stable,
   versioned SoA view — instance transforms, mesh/material ids, lights, camera —
   and the provider does its own culling, sorting and submission. One crossing per
   frame. This is also exactly the shape GPU-driven rendering wants, which is why
   it converges with `docs/plans/rhi-design.md` rather than competing with it.
3. **Window-and-device handoff**: the provider owns the swapchain too; the engine
   hands it the scene and a camera. Maximum freedom, and the editor's viewport
   compositing becomes the provider's problem.

Recommendation: **(2), and not before the RHI work forces the scene representation
to be explicit anyway.** Attempting a renderer provider ABI on top of today's
bgfx-leaking `RenderContext` would produce a seam in the wrong place that we then
have to keep.

## 5. What must NOT be replaceable, and why saying so is part of the answer

If everything is a provider, there is no engine — just a launcher with opinions
about file formats. The line I would draw:

- **The ECS is the engine.** Entities, components and the scene graph are the shared
  vocabulary every seam is expressed in. Making it swappable makes every ABI
  parameterised over an unknown data model.
- **The asset and cook pipeline is the engine.** The DDC, content addressing,
  dependency invalidation. A provider can consume cooked bytes; it does not get to
  redefine what cooking means.
- **The frame's phase order is the engine.** Somebody has to decide that physics
  steps before animation reads transforms.

And a cost worth stating plainly, because it is charged every week rather than
once: **every provider seam slows feature velocity.** A new capability must either
be expressible in the ABI, or become provider-specific, or force a version bump.
Three seams is a tax on every future feature. That is the price of no lock-in and
it is worth paying — but it should be paid deliberately, on the subsystems where a
studio genuinely has an existing investment (audio middleware, a licensed physics
engine, an in-house renderer), not on everything.

## 6. The lock-in axes an ABI does not fix

Worth naming, because these are what studios actually get burned by, and two of
them are cheaper to fix than any of the above:

- **Data.** If a studio cannot read their own cooked assets without our tools, they
  are locked in no matter how many ABIs exist. The formats are documented in
  `modules/assetlib`; what is missing is a *stated guarantee* and an exporter.
- **The game loop.** Can a studio run their own `main()` and drive the engine, or
  must they use ours? `engine_host` suggests the former; it should be a documented,
  tested contract rather than an accident.
- **The editor.** Can they ship without it, and script their build? `engine_build`
  says yes; that should be load-bearing and tested, not incidental.
- **The primitive tier** (`jobs`, `memory`, `drawSubmit`) is the other half of this
  story and arguably the more important half. **Providers let a studio replace what
  exists; primitives let them build what does not.** A studio whose thing the engine
  never modelled needs a job pool, a tagged allocator and a way to get pixels on
  screen — not permission to reimplement our animator.

## 7. Order of work

1. **Audio to seam B, completely** (host loader; miniaudio *through* the ABI; the
   A/B measurement). One subsystem done properly is the template and the proof.
2. **Write the A/B harness once**, so every later provider inherits it.
3. **Physics ABI**, with the character controller deliberately left out of the core
   group and determinism as a capability bit.
4. **Document the data / loop / editor guarantees** — cheap, and it addresses the
   lock-in that ABIs do not.
5. **Renderer, after the RHI work**, at scene-handoff granularity.

Nothing here should start before step 1 finishes. The single most likely failure
mode for this whole plan is three half-built ABIs, none of them exercised by the
implementation that ships — which is the state `IRenderPipeline` is already in.
