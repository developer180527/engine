---
status: decided
id: DR-0011
title: Capacity grows with content; nothing is reserved for content that does not exist
date: 2026-10-01
source:
  - src/plugins/info.md
---

## Decided
An empty game (no assets, no scripts, only the main loop) costs close to
nothing, and a minimal 2D game does not carry the 3D engine. Every capacity,
pool, allocator and render target is created when content needs it and grows
with it: the physics world on the first body, rebuilt larger as content
outgrows it; the shadow map on the first shadow-casting light; the editor's
scene targets on first use. `empty_game_budget_test` holds the line.

## Rejected
Sizing for a large 3D game at startup, as WO-048 did for physics (65 536
bodies, a 64 MB temp allocator) and as the renderer did for the shadow map and
the editor targets, even when the game draws neither. Also rejected: simply
picking smaller fixed numbers, which would break WO-048's 2 000-box piles.

## Why
The owner's rule (2026-10-01): developers' scripts will be expensive, so the
engine's own idle cost must be negligible. Measured on an empty project before
WO-050: 205 MB mapped (physics 78 MB, bgfx 86 MB, render targets 72 MB) for a
frame doing 0.15 ms of work. After: 95.8 MB, physics 0, and the shadow map and
editor targets only when used.

## What would reverse it
Nothing in the engine's own systems. A specific allocation may be made eager
when measurement shows the lazy path costs a frame hitch a game would notice
(first-use creation of a large target, say), and then it is eager for that
reason, recorded, and still sized to what is used.
