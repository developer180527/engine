---
status: decided
id: DR-0006
title: No frozen renderer ABI (no third-party binary renderer), now or soon
date: 2026-09-05
source:
  - docs/rhi/swappability.md
---

## Decided
Of the four meanings of "swappable renderer", the engine builds three
(backend under the RHI, `IRenderPipeline`, renderer-as-library) and does
NOT build the fourth: a third-party renderer shipped as a binary plugin
against a frozen C ABI.

## Rejected
Freezing a renderer ABI so studios can ship `renderer.so` against the engine.

## Why
Dispatch cost is not the obstacle (a cross-dylib call measured 0.68 ns,
0.059 ms a frame at 100 000 draws). Permanence is: a frozen ABI can never be
un-frozen, a shipped group's size and offsets are forever, and freezing now
would fix the CPU-driven shape into a permanent contract at exactly the moment
it is about to change (P3, GPU-driven). It is the one-way door.

## What would reverse it
The renderer's shape settling (after GPU-driven, G6) AND a real external
consumer asking for it. Until both, keep the option, not the ABI.
