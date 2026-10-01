---
status: decided
id: DR-0008
title: bgfx runs single-threaded: no render thread
date: 2026-08-04
source:
  - src/render/renderer/device.cpp
---

## Decided
`renderFrame()` is called before `bgfx::init`, so bgfx renders inline on the
main thread at `bgfx::frame()`.

## Rejected
bgfx's default: a separate render thread pipelining submit and render.

## Why
Measured over three runs of `engine_host --frames 600`: single-threaded,
mean cadence 8.33–8.36 ms with worst frames 16–30 ms; multithreaded, two of
three runs stalled for ~1 SECOND (Metal drawable acquisition starving once
submit and render are pipelined). And there was nothing to overlap: per-frame
CPU work was 0.38 ms of an 8.33 ms period. Pipelining also adds a frame of
latency by construction, against the motion-to-photon budget.

## What would reverse it
The main thread's own CPU work approaching the frame period (FrameStatsChannel
`work` near the display period), so there is submit cost to overlap; then
re-measure with RenderStats' waitSubmit/waitRender.
