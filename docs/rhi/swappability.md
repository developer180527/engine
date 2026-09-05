---
status: decided
covers:
  - src/render/
---
# What "swappable renderer" means, and what each version costs

> **The question this answers:** the renderer must be swappable *and* must not
> lose performance. Can both be true, and if not, which do we drop?
>
> **The short answer:** both are true, but only because "swappable" means four
> different things and only one of them is expensive. We should build three of
> them and deliberately NOT build the fourth.

## 0. The finding

**Dispatch cost is not the problem, and this is measured, not assumed.**

A cross-`.dylib` indirect call on this machine costs **0.68 ns**. At a hundred
thousand draws a frame — more than this engine has ever issued — that is
**0.059 ms/frame** of call overhead. On a 120 fps budget that is 0.7% of a frame,
and the engine is nowhere near 100 000 draws (50 000 real props submit in 299).

So "swappable costs performance" is false *as an argument about function calls*.

**What does cost performance is the semantic floor of the abstraction** — what it
refuses to let you express — and bgfx is the proof, in this repo, today: no
bindless, a per-draw uniform model you cannot opt out of, and a 4 096-draw
ceiling guarding a missing bounds check. None of those are dispatch. All of them
are the abstraction being the *lowest common denominator of twelve APIs, several
of them legacy*.

That distinction is the whole document, and it points at a specific conclusion:
an abstraction over **two modern explicit APIs** (Metal 4 + Vulkan 1.3, which
[`design-axioms.md`](design-axioms.md) axiom 6 already picked) has almost no
floor, because those two agree about almost everything that matters.

## 1. Four things called "swappable"

They are routinely conflated, they have wildly different costs, and only one of
them needs an ABI.

| | What is swapped | Boundary | Crossings/frame | Cost | Verdict |
|---|---|---|---|---|---|
| **A** | **Graphics backend** — Metal 4 vs Vulkan under one RHI | internal virtual or compile-time | **1** (see §4) | **zero** | **build it** |
| **B** | **Render pipeline** — forward vs deferred vs a game's custom passes | C++ virtual (`IRenderPipeline`) | 1 per view | **zero** | **already exists** |
| **C** | **Renderer as a reusable library** — vCAD links it and drives it | source / static link | n/a — no runtime boundary | **zero** | **design for it** |
| **D** | **Third-party renderer as a binary plugin** — a studio ships `renderer.so` against a frozen engine | frozen C ABI | 1 per frame, if done right | small, but **permanent** | **do NOT build now** |

**The load-bearing distinction: C does not require D.**

vCAD is your own product. It can link the renderer at build time, with LTO,
inlining, and no ABI at all. "Reusable in another application" and "loadable as a
binary plugin from a third party" are completely different requirements, and only
the second one is expensive — not because of speed, but because **a frozen ABI is
irreversible.** See §6.

## 2. The measurement

**The harness is `tests/perf/seam_cost_bench.cpp`, in the `perf` lane:**

```bash
ctest -L perf            # or: ./seam_cost_bench 500
```

It was prose-only when this document was first written, which review correctly
called out against §8's own rule — the three precedents §8 cites all have
committed harnesses. It is not a gate: dispatch cost is a property of the
machine and the compiler, not of this repo, so a threshold would fail on
somebody's laptop for no defect.

Apple M-series, `-O2`, 100 000 calls/frame × 500 frames, with a data-dependent
body so the optimiser cannot solve the loop in closed form:

```
  direct (inlinable)                  0.08 ns/call    0.0083 ms/frame
  virtual, monomorphic                0.67 ns/call    0.0672 ms/frame
  virtual, polymorphic (2 impls)      0.85-1.30 ns    0.085-0.130 ms/frame
  function pointer, same binary       0.08 ns/call    0.0084 ms/frame
  function pointer, across .dylib     0.67 ns/call    0.0672 ms/frame

  cross-.so penalty vs direct:  +0.059 ms/frame at 100k draws
```

**Two notes on reading it.** The cross-`.so` penalty is stable to three decimal
places across runs (0.0589 / 0.0595 / 0.0597 / 0.0592) — that is the
load-bearing number. The **polymorphic row is the noisy one**, swinging 0.85 to
1.30 ns as the branch predictor's luck changes; it is reported as a range rather
than a figure because a single sample of it would be a lie.

**The harness forces `-O2` on itself regardless of build type**, and that is not
a preference. The default dev build here is Debug, and at `-O0` nothing inlines,
so every variant costs the same. The first Debug run of this harness reported a
cross-`.dylib` call as *faster than a direct one* — the shape of a measurement
that is measuring build flags instead of the thing.

Three things worth reading off it:

* **A monomorphic virtual call and a cross-`.so` call cost the same** (0.68 ns).
  The branch predictor handles a single-implementation vtable perfectly; what you
  actually pay for, in both cases, is **lost inlining** — which is why the direct
  row is 8× faster here, having been vectorised.
* **This corrects our own document.** [`../architecture/provider-abi.md`](../architecture/provider-abi.md)
  §1 estimates a per-draw renderer ABI at "~0.2 ms/frame just in call overhead".
  The measured figure is **~0.06 ms/frame — 3.4× lower**. The estimate was
  pessimistic, and the conclusion it supported needs a better argument than the
  one it had. §5 is that argument.
* **The microbenchmark flatters the ABI**, and it should be read with that in
  mind: the body is four instructions. A real draw call marshals state, and the
  lost inlining is of *real* work. Treat 0.059 ms as a floor, not an estimate.

## 3. Where the performance actually goes

Five distinct costs get bundled into "abstraction overhead". Only two of them
are large, and neither is dispatch.

| # | Cost | Size | Avoided by |
|---|---|---|---|
| 1 | **Call dispatch** | 0.68 ns × crossings | putting the seam at a coarse granularity (§4) |
| 2 | **Lost inlining** | folded into #1 above | same |
| 3 | **Marshalling / data layout** | can be large | bulk SoA handoff, never per-object accessors |
| 4 | **Semantic floor (LCD)** | **large — this is the real one** | abstracting two *similar, explicit* APIs, not twelve |
| 5 | **Hidden portability work** | **large — GPU time, not CPU** | not hiding barriers; a render graph derives them from declarations |

**#4 and #5 are where bgfx costs us**, and they are worth being concrete about
because they are the live example:

* bgfx cannot express bindless, so GPU-driven submission is unreachable — an
  indirect draw cannot bind a texture per draw. That is a *capability* the
  abstraction refuses, and no amount of call-overhead cleverness recovers it.
* bgfx tracks resource state implicitly, so it must be **conservative with
  barriers**. That is GPU time spent on nothing, charged every frame, and it is
  invisible to any CPU profiler.

Both are consequences of covering GL 2.1, D3D9 and Metal with one vocabulary.
Neither is a consequence of "being an abstraction".

## 4. How shipping engines place the seam

Rung 2–3 evidence ([`workflow.md`](workflow.md) §1) — read from source in this
tree, and from the APIs' own documentation.

**bgfx is the existence proof, and it is vendored right here.** It supports a
dozen backends, chosen at *runtime*, through a virtual `RendererContextI`. The
number of virtual calls it makes per draw is **zero**. `Encoder::submit`
(`bgfx.cpp:4192`) records into a command buffer; the backend interface is entered
exactly **once per frame**, at `bgfx.cpp:2704`:

```cpp
m_renderCtx->submit(m_render, m_clearQuad, m_mipGen, m_textVideoMemBlitter);
```

One virtual call, handed the entire recorded frame. **That is the pattern**: a
command buffer converts a per-draw boundary into a per-frame one, and the cost of
runtime backend swappability collapses to nothing.

This is not bgfx being clever; it is the standard answer.

* **Unreal** records into `FRHICommandList` and translates the list on the RHI
  thread, rather than issuing virtual RHI calls per draw from the game thread.
  The backend (D3D12 / Vulkan / Metal) is selected at startup, and it ships AAA
  titles at that granularity.
* **Unity** puts its pipeline seam (SRP) at *pass* granularity in C#, with the
  device abstraction below it in C++ — nobody crosses a boundary per draw.
* **id Tech** is the honest counterexample: Doom Eternal has **no** backend
  abstraction, Vulkan only, and is among the fastest renderers shipped. Proof
  that zero abstraction is viable — and also that it costs you every other
  platform, which is a trade we cannot make with vCAD on iPad.

**Our own `IRenderPipeline` is already at the right granularity**: `render()` is
called once per view (`targets.cpp:91/108/123`) — one to three times a frame.
Its dispatch cost is unmeasurable and always will be.

## 5. The argument that actually kills a per-draw renderer ABI

Not overhead. **Architecture.**

The target renderer is GPU-driven ([`design-api.md`](design-api.md) §4.3): a
compute shader culls, writes indirect arguments, and the GPU issues the draws.
**In that architecture the CPU does not make per-draw calls at all** — there is
nothing per-draw for an ABI to be slow at, because there is no per-draw CPU work
to cross a boundary.

So a `submitDraw`-per-object provider ABI is not merely expensive; it is a
*model of rendering the engine is deliberately leaving behind*. Building one
would freeze the CPU-driven shape into a permanent contract at exactly the moment
we are trying to escape it.

That is the real reason for
[`../architecture/provider-abi.md`](../architecture/provider-abi.md) §4.3's
recommendation of **scene-handoff granularity** — one crossing per frame, a
stable SoA view of instances, lights and camera — and it is a much stronger
reason than the call-overhead estimate that section actually gives.

## 6. What is irreversible

The question that matters most, since the stated fear is discovering
architectural debt later. Sorted by how hard each is to undo.

| Decision | Reversible? | Why |
|---|---|---|
| **Freezing a renderer ABI** | **NO — permanent** | A frozen ABI can never be un-frozen; `extension-model.md`'s whole discipline is that a shipped group's size and offsets are forever. This is the one-way door. |
| **The scene representation the renderer consumes** | **Barely** | Every consumer and every pipeline is written against it. This is the *real* interface, whether or not it is ever an ABI. |
| **Who owns the swapchain / window** | Hard | Decides whether the editor's viewport compositing is ours or the provider's. Open decision 5 in `platform-embedder.md`. |
| Compile-time vs runtime backend selection | **Easy** | An implementation detail behind the same header. |
| Virtual vs direct dispatch inside the renderer | **Easy** | Devirtualisable, and measurable if it ever matters. |
| Which backends exist | Easy | Additive. |

**The conclusion follows directly:** the expensive, irreversible thing is
freezing an ABI — so do not freeze one. The valuable, also-hard-to-change thing
is the *scene representation* — so spend the design effort there, where it pays
off whether or not an ABI ever exists.

Getting the scene handoff right is not speculative work: GPU-driven rendering
needs exactly that representation anyway (a persistent, versioned instance buffer
with stable ids). **The RHI programme forces it regardless.** That is why §7
recommends doing it and stopping there.

## 7. What to do

1. **Build A** — the RHI, backend swappable, seam at frame granularity, designed
   against two explicit APIs so the semantic floor stays low. **No performance
   cost, and this is the existing plan** ([`phases.md`](phases.md) G2–G4).
2. **Keep B** — `IRenderPipeline` as-is. It is already free and already the right
   granularity. G1b made it declarable without a graphics API in scope.
3. **Design for C** — a clean library boundary so vCAD can link the renderer.
   No ABI, no dispatch cost, full LTO. This is the reuse the programme is for
   ([`../plans/renderer-program.md`](../plans/renderer-program.md) §1: the reuse
   boundary sits *below* the renderer).
4. **Do NOT build D.** No frozen renderer ABI, now or soon. Keep the *option*
   open by making the scene handoff explicit and versioned — which item 1 forces
   anyway — and revisit only if an actual third party turns up wanting to ship a
   renderer against a frozen engine binary. Nobody does today.

**Nothing is dropped by this.** Every capability the original question asked for
— swappable backend, tunable pipeline, reusable in vCAD — is delivered by A, B
and C at zero measured cost. Only D is deferred, and D is the one nobody has
asked for.

## 8. The gate that keeps this honest

> **No abstraction ships without being measured against a direct call to the
> backend on the same scene.**

This repo has the instruments (`rdiag::SubmitStats`, GPU timings, the profiler)
and the precedent both ways: NEON was declined at 0.4% of a phase; the residency
sweep was fixed at 0.463 ms; the thread-QoS hypothesis was half-refuted by its
own test. The 0.7%-of-frame figure in §2 is in the same range as the NEON
measurement that was declined — so it is a real number, not a free pass, and the
answer here is "yes" only because the seam sits where the crossings are ~1 per
frame rather than 100 000.

**If a future measurement shows the abstraction costing GPU time** — conservative
barriers, a fast path it cannot express — that is cost #4 or #5 from §3, and the
answer is to fix the abstraction's vocabulary, not to accept the loss. The
trigger for abandoning the whole idea is in
[`decision-record.md`](decision-record.md) §10 and is unchanged.
