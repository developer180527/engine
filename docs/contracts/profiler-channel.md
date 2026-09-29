---
status: as-built
contract: profiler-channel
kind: interface
state: provisional
owner: src/core
header: src/core/profiler.h
implementations:
  - real: src/runtime/frame_stats_channel.h#FrameStatsChannel
  - real: src/render/render_stats_channel.h#RenderStatsChannel
  - real: src/runtime/input/input_latency_channel.h#InputLatencyChannel
covers:
  - src/core/profiler.h
verified: 2026-09-27
---

# profiler-channel — a subsystem's own numbers, in the profiler

`prof::IProfilerChannel`: a named stream of per-frame measurements a subsystem
publishes (frame stats, render submission stats, input latency) without the
profiler knowing about the subsystem.

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
