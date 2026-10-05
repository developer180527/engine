---
status: decided
id: DR-0012
title: The RHI is built as a standalone, reusable library; bgfx is not its upper bound
date: 2026-10-05
source:
  - docs/rhi/phases.md
---

## Decided
The engine gets its own explicit, bindless RHI (Metal 4 and Vulkan 1.3,
abstracting only where the APIs' models differ, study 007), and it is built as a
library that stands on its own: no engine includes, its own tests, samples and
validation, a C ABI. The engine is its first consumer, not its owner. The G0a
spike no longer decides *whether* to build it. It becomes a baseline: what the
bgfx path costs at 50 000 objects, so the RHI has a number to beat.

## Rejected
Gating G2–G6 on G0a, as `phases.md` did ("it decides whether G2–G6 are worth a
year"). Also rejected: an RHI written as engine-internal code, shaped only by
this renderer.

## Why
bgfx has no bindless: an indirect draw cannot pick its own textures, so a fully
GPU-driven textured scene is not reachable on it at any amount of work (it can
cull and draw indirectly, but the material still binds per draw,
`evidence-bgfx.md` §2.2). Its per-draw uniforms, the 8 MB uniform ceiling,
preallocated command buffers and state tracking the caller cannot turn off are
its design, not settings (axioms 1, 2 and 4). And the owner's reason, which no
spike measures (2026-10-05): the library is meant for several projects and for
other developers, so its value does not depend on this engine's frame time.

## What would reverse it
The owner deciding the RHI serves only this engine, or the engine dropping
GPU-driven rendering as a goal. A G0a result cannot reverse it; it can only
change what the RHI must beat.
