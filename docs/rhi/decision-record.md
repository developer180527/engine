---
status: decided
covers:
  - src/render/
---
# Should we replace bgfx? The standing answer

*Original `rhi-design.md` §0 and §10. Split out 2026-09-04; wording preserved.*

> **Status: proposed, nothing built.** No `verified:` date, because there is
> nothing as-built to verify. This is a decision and its rationale, written to be
> argued with.

## 0. The answer, up front

**Yes — but not for the reason in the question, and not first.**

"bgfx is a lowest common denominator over a dozen APIs, most of them legacy" is
true and it is not what is costing us anything today. The measured bottleneck in
this engine is `Render.extract` — **CPU 24.8 ms against GPU 9.11 ms at 50 000 real
props** (`src/render/issues.md` R20). An RHI rewrite does not touch that 18.8 ms
of extraction. Done in the wrong order, this project is four months of work for a
frame time that does not move, and the repo has a standing rule against exactly
that kind of change (NEON was declined on a 0.4%-of-frame measurement).

The argument that does hold is stronger than "bgfx is old":

> **The only way to stop being CPU-bound is to stop doing per-object work on the
> CPU. That requires GPU-driven submission — compute culling into indirect
> arguments, with bindless resources and no per-draw uniforms. bgfx has the
> compute and the indirect draws. It cannot give us bindless, and its per-draw
> uniform model is not something you can opt out of.**

So the project is not "a modern bgfx". It is **the substrate that lets the render
path stop scaling with entity count on the CPU**, and ray tracing, mesh shaders
and the rest arrive as consequences of the same design rather than as features
bolted on. Framed that way it is justified by our own numbers, and the phase order
in [`phases.md`](phases.md) falls out of it.

## 10. What would make me say stop

The exit conditions matter as much as the argument, and they are written here
rather than discovered in month five.

- **G0 shows the GPU is nowhere near the limit on discrete hardware either.**
  Then the answer is incremental extraction and job-system work, not an RHI.
- **G4 lands and the win is only the draw ceiling.** That is a real but modest
  prize for three months; reassess before G5.
- **The shader toolchain (G3) slips past ~6 weeks.** That is the classic sinkhole
  and the honest signal to reconsider scope.

## The property that makes this decision cheap to hold

[`evidence-coupling.md`](evidence-coupling.md) §2.1 carries it, and it is the
single most important thing about this plan:

> **Its early phases are valuable independently of its late phases.**

De-contamination (G1) is the same refactor the headless dedicated server needs
and the same one an embedding host needs. So the project can be abandoned at any
point before G2 with the work already banked, which is why "yes" is a safe answer
to give this early.
