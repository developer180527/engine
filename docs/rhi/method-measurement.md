---
status: decided
covers:
  - src/render/
---
# Which of our numbers can be trusted, and what that gates

*Original `rhi-design.md` §3 and §3.1. Split out 2026-09-04; wording preserved.*

> This is the methods document for the whole directory. Read it before citing any
> renderer number, here or elsewhere.

## 3. The measurement trap, which is the sharpest point in the question

> **Corrected 2026-08-28 — this section's conclusion was wrong, and axiom 6
> with it.** What follows about SoC measurement remains true. What did NOT survive
> is the inference drawn from it: "stop pretending the Mac is a performance
> target." **Apple Silicon is a SHIPPING target**, because the second consumer of
> this renderer — vCAD — ships on macOS and iPad, where Metal is the only backend
> that exists. See §3.1.

**"On SoCs like Apple's, one measurement gives false positives."** This is correct
and it invalidates part of our own roadmap, so it deserves to be stated plainly:

Every renderer number in this repo was measured on an M-series Mac — unified
memory, no PCIe transfer, tile-based deferred rasterisation, no discrete VRAM
budget. On that machine:

- upload cost, staging buffers and residency are close to free, so the entire
  streaming and eviction design is measured in its best case;
- bandwidth-bound passes behave unlike a discrete GPU's;
- and crucially, **GPU-driven rendering's whole payoff — moving per-object work off
  a CPU that is feeding a bus — is the thing an SoC hides.**

So: **a measurement rig on real PC hardware is required before any D3D12 or Vulkan
performance claim is falsifiable.** The testing farm (headless x86 Debian, SQLite
results DB) has no GPU lane, and this repo's entire method is falsifiability.

## 3.1 What that argument does NOT license

The original version of §3 concluded that the Mac should stop being a performance
target and that **nothing** could start before the farm had GPUs. Both halves need
correcting, and the second one has been holding up work that needs no farm at all.

**Apple Silicon is a shipping target.** This renderer has two consumers:

| | platforms | backends available |
|---|---|---|
| the game engine | Windows, Linux, macOS (editor) | D3D12, Vulkan, Metal |
| **vCAD** | **macOS and iPad** | **Metal, and nothing else** |

vCAD hosts 50 000-part assemblies on an iPad. That is the weakest CPU in the
entire picture driving the largest object count, which is precisely the workload
GPU-driven submission exists for — so the payoff is *largest* on the platform the
old axiom 6 called dev-only and allowed to be slow.

**And measurement gates performance claims, not structural ones.** De-contamination
([`evidence-coupling.md`](evidence-coupling.md) §2.1), opaque handles, a retained
scene, stable object ids — every one of those is falsifiable today with link-time
assertions and headless builds. G0 gates G4–G8. It does not gate G1, and treating
it as though it did is why the five files in §2.1 are still uncleaned.

## The standing rule this leaves behind

> **A number measured only on Apple Silicon may support a structural argument and
> may not close a performance claim about discrete hardware.**

That is why [`phases.md`](phases.md) splits G0 into two: **G0a** is the bgfx spike,
which needs no farm, and **G0b** is the GPU measurement lane, which gates only the
phases that make performance claims.
