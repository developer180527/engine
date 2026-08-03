---
status: as-built
tier: working
verified: 2026-08-04
covers:
  - src/render/diag/
tests:
  - tests/render_diag_test.cpp
---
# Render diagnostics


## Submission counters
`printSubmitStats` reports `rdiag::SubmitStats` (render/submit_stats.h), filled by
ForwardPipeline. It exists because bgfx's Noop backend never sets `numDraw`, so
draw counts could not be asserted headlessly — counting on our side works on every
backend. Each counter states an audit finding directly: bone uploads vs skinned
ITEMS (R4, and the printer WARNS when they diverge), material binds vs draws (R7),
batch runs vs draws (R5). Shadow draws are counted separately because that pass
walks every item rather than a culled set, so `shadowDraws == itemsConsidered` is
the signature of that bug.

## Frame timing (GPU, CPU, waits)
`FrameGpuStats` also carries what `bgfx::getStats()` measures and nothing read
until now: GPU ms per frame, CPU frame ms, and `waitSubmit`/`waitRender`. Draw
counts say how much the GPU was ASKED for; GPU time says what it cost, which is
what decides whether a submission change is worth making.

Three properties are encoded rather than hidden:
- **An unsupported GPU timer reports UNAVAILABLE, never 0.00 ms.** A fabricated
  free GPU is worse than no number, so `gpuTimedFrames()` gates the average and
  only measured frames feed it.
- **GPU times LAG the CPU frame** (bgfx reports `gpuFrameNum` for the frame they
  belong to). Fine for averages, wrong for correlating one specific frame.
- **`waitSubmit`/`waitRender` read ~0 today** because `renderer.cpp` calls
  `bgfx::renderFrame()` before `bgfx::init` to force single-threaded mode. They
  are collected anyway: they are exactly what would price re-enabling the render
  thread.

Measured on fps_shooter, 600 frames (2026-08-03): GPU avg 2.66 ms / max 5.70,
CPU frame 11.16 ms, 13 draws, 0 handle churn.

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
