---
status: decided
id: DR-0007
title: Cull bounds are one interleaved {x,y,z,r} stream, not planar arrays
date: 2026-08-04
source:
  - src/render/world/render_world.h
---

## Decided
`CullStreams` stores each item's world bounding sphere as one interleaved
16-byte `CullSphere {x,y,z,r}`. The retained scene table (`render_scene.h`)
keeps the same layout.

## Rejected
Four parallel arrays (x[], y[], z[], r[]) or Filament-style planar per-axis
AABB columns, for SIMD-friendly loads. Proposed again by the 2026-09-22
engine survey for the retained scene (WO-020).

## Why
Measured: over 100 000 spheres the scalar plane test costs 1.47 ns per item,
while the cull's real in-engine cost is ~33 ns per item, so the arithmetic a
planar layout speeds up is ~4% of the phase. What does pay is stream count:
one sequential 16-byte read measured 1.27x faster than four independent
streams (prefetcher and TLB). A 2.3x NEON win on the arithmetic would have been
~0.4% of the render path.

## What would reverse it
A cull whose arithmetic dominates (GPU-driven culling on the CPU side, or
many more planes per test), measured; or a SIMD cull that wins on the
interleaved layout (`vld4q_f32` deinterleaves it in one instruction: 1.98x,
not worth it yet).
