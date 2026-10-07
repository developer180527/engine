---
status: plan
id: WO-062
title: bgfx is sized for what the host draws, not for its defaults (about 49 MB down to about 21 MB)
program: renderer
priority: P1
size: S
state: todo
depends: []
touches:
  - src/render/renderer/device.cpp
  - CMakeLists.txt
  - tests/empty_game_budget_test.cpp
  - docs/work/WO-050-empty-game-costs-nothing.md
source: review 2026-10-07, items 2 and 3 (every bgfx allocation over 256 KB, logged under the debugger)
---
## Why
An empty project holds 49 MB in bgfx, and about 30 MB of it is two `bgfx::init` settings nobody set.

WO-050 left the bgfx heap at 49 MB without asking which of it can be sized at run time. Most of it can:

| what | size | sized by |
|---|---|---|
| uniform buffers, 8 encoders × 2 frames × 1 MB | 16 MB | `init.limits.maxEncoders`. The engine renders on one thread (DR-0008) and ImGui uses one encoder, so 1 does. Saves 14 MB |
| transient vertex and index buffers | 16 MB | `init.limits.transientVbSize`, `transientIbSize`. The player uses them for debug lines and one 3-vertex triangle; the editor's ImGui needs more. Saves about 14 MB in the player |
| per-frame tables for 16 383 draws | 14.5 MB | compile time (`BGFX_CONFIG_MAX_DRAW_CALLS`). The pipeline caps at 4 096 draws |
| Metal context, uniform cache | 2 MB | fixed |

The draw tables also cost time: every frame, even an empty one, clears and sorts arrays sized for 16 383 draws.

## Done when
- [ ] `maxEncoders = 1` in every host, with a comment citing DR-0008
- [ ] transient buffer sizes set per host: the player and the server small (sizes measured from the real users, with headroom written down), the editor at what ImGui needs
- [ ] **running out is handled, never a crash**: every transient allocation checks what is available first (`getAvailTransientVertexBuffer`/`IndexBuffer`), drops what does not fit, and reports it once per frame like the external-draw ceiling does. A test that draws more debug lines than fit shows the drop and the report
- [ ] `BGFX_CONFIG_MAX_DRAW_CALLS` lowered to the pipeline's 4 096 cap **if** the 50k scene is unchanged (draws, shadow draws, extraction time, GPU time), measured before and after. If it is not, the reason goes in the log and the value stays
- [ ] measured after: the bgfx heap for an empty player project and for the editor, and the per-frame cost of an empty frame
- [ ] the windowed budget check (WO-061's gate, or its own if that lands later) fails when the player's bgfx heap exceeds the measured figure plus a written margin. Restoring the defaults turns it red, checked
- [ ] WO-050's log corrected: the remaining bgfx heap was mostly sizeable at run time

## Contract
Nothing: a host that sets no sizes gets bgfx's defaults, as today. Out of transient space, a draw is dropped and reported, never asserted on.

## Not in scope
Memory that grows with content: that is the custom RHI's job (DR-0012). This order only stops reserving for content an empty game does not have.
