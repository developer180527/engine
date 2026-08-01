---
status: as-built
tier: working
verified: 2026-08-01
covers:
  - src/render/diag/
tests:
  - tests/render_diag_test.cpp
---
# Render diagnostics

## Purpose
Answer three questions about the renderer with numbers instead of opinion:

1. **Is it allocating when it should be steady?** (handle churn)
2. **Does this frame fit the machine we ship to?** (budget)
3. **What is resident, who owns it, and did anything leak?** (census)

Phase 2 of `docs/plans/renderer-audit-and-plan.md`. These reports were impossible
before the GPU resource cache existed — not because nobody wrote them, but
because "unused" has no meaning without refcounts (audit finding R1).

## One concern per file
The previous version of this was a single 151-line header doing sampling,
judging, budgeting and printing at once. Split:

| file | concern | needs bgfx? | testable headless? |
|---|---|---|---|
| `frame_gpu_stats.{h,cpp}` | sample bgfx counters, judge churn | yes (sampling only) | yes, via `sampleExplicit` |
| `gpu_budget.{h,cpp}` | target tiers, ceilings, PASS/OVER | no | yes |
| `resource_census.h` | resident resources, owners, duplicates, leaks | no | yes |
| `diag_report.h` | formatting | no | n/a |
| `../render_stats_channel.h` | profiler adapter | yes | n/a |

The split is what makes the subsystem testable at all: `src/render` has no GPU
test harness, so everything worth asserting is deliberately kept in the
GPU-free half. `FrameGpuStats::sampleExplicit` is the seam that lets churn
logic be driven from a test with no device.

## Churn — the diagnostic this exists for
A steady scene holds constant handle counts: content is loaded, so moving the
camera should create and destroy nothing.

- counts **rising** → leak
- counts **oscillating** → per-frame create/destroy
- counts **flat** → allocation traffic is transient buffers (ImGui, debug
  lines), which is by design — read `transientVb/Ib` instead

The delta is the signal. The absolute number never told anyone anything, which
is why the verdict, not the count, is the headline.

## Budget tiers
`TargetTier::Low` is not hypothetical — it is the Intel UHD 630 / 4 GB machine
this engine is validated against, and it is the tier that decided the renderer
is forward rather than deferred (`docs/architecture/renderer-architecture.md` §2).

Sub-limits deliberately sum to less than the total (80 MB of 128 MB): a budget
with no slack is one you blow on the first feature. They are also what push a
project to the right setting rather than letting one allocation eat everything
— fps_shooter's default 2048 shadow map is 23 MB of render targets, which is
18% of total VRAM but **over** the 20 MB render-target ceiling. That is the
signal to ship low-end at `shadowResolution: 1024` (11 MB), and it is asserted
in the test.

An unknown or missing tier string parses to `Low`, the strictest — guessing
generously is how a budget check quietly stops catching anything.

## Usage
```bash
engine_player <project> --frames 400 --gpu-stats            # measure
engine_player <project> --frames 400 --budget low           # verdict; exit 4 if OVER
```
`--budget` exits non-zero when over, so it can gate a packaging step or run on
the low-spec machine itself and fail loudly there.

## Known limitations
- The census reports what is in `GpuResourceCache`, and the cache is currently
  wired only into the synchronous cooked-texture path. Until the async mesh
  path routes through it too, a census of a running scene under-reports.
- `suspectedDuplicates` infers from (size, owner) pairs; with content keying
  working it should always be empty, so treat a hit as a regression signal
  rather than a precise identification.
