---
status: as-built
contract: gpu-seam
kind: functions
state: provisional
owner: src/render
header: src/render/gpu.h
implementations:
  - real: src/render/gpu.cpp
  - null: src/render/gpu_null.cpp
tests:
  - tests/gpu_seam_test.cpp
covers:
  - src/render/gpu.h
verified: 2026-09-27
---

# gpu-seam — the renderer's own GPU vocabulary

Free functions over opaque handles (`gpu::TextureHandle`, `gpu::ColourSpace`, …)
so render code outside the bgfx backend names no bgfx type. One implementation
is linked: bgfx, or the null one for server builds. `check_gpu_seam.py` gates
new bgfx includes outside the seam.

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
Not yet written.
