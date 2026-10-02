---
status: plan
id: WO-054
title: One GPU bone buffer per frame — no per-draw palette upload
program: renderer
priority: P3
size: L
state: parked
parked-until: the RHI reaches G4 (per-draw data in a structured buffer, WO-027); building it on bgfx means building it twice
depends: [WO-053]
contracts: [renderer, render-pipeline]
touches:
  - src/render/pipeline/opaque_pass.cpp
  - shaders/vs_skinned.sc
source: review 2026-10-03 of WO-040
---
## Why
Every skinned draw uploads its palette as a uniform, and Metal's uniform scratch is why the renderer caps itself at 4 096 draws.

## Done when
- [ ] all animated poses are written once per frame to one GPU buffer, and the skinning shaders read bones by offset plus the section's bone map
- [ ] skinned crowds can be instanced, and the shadow and main passes share one upload
- [ ] WO-053's uniform path remains the fallback where the GPU cannot read buffers or textures in the vertex stage
- [ ] measured against WO-053 on the same 100-character scene

## Contract
Nothing: until this lands, WO-053's per-draw gather is the path, and it stays the fallback after.

## Not in scope
Compute pre-skinning reused across passes; a later order.
