---
status: plan
id: WO-004
title: The roadmap stops lying about where we are
program: process
priority: P1
size: S
state: done
done: 2026-09-30
evidence: docs/process/roadmap.md rewritten as status decided; docs/README.md start-here; WO-030 and WO-031 carry its open items
touches:
  - docs/process/roadmap.md
  - docs/README.md
source: found 2026-09-22 — roadmap says 20 subsystems/6 hardened, ENGINE_STATUS says 22/9
---
## Why
`docs/README.md` tells you to read the roadmap second, and its numbers are a month old.

Nothing caught it: its `covers: docs/` points at a directory rather than the code its claims are about, so the staleness check has nothing to compare against.

## Done when
- [x] the roadmap's live numbers are removed and replaced with a pointer to `ENGINE_STATUS.md`. A copied number is a number that will go stale.
- [x] its "what next" ordering is replaced with a pointer to `docs/work/BOARD.md`. The roadmap keeps strategy: what is missing, and why that order.
- [x] `docs/README.md` "start here" reads: `work_orders.py brief`, then `ENGINE_STATUS.md`, then the board, then the roadmap
- [x] the roadmap's `covers:` names something that actually changes when it should be re-read, or it becomes `status: decided`, so staleness is honest either way

## Log
- 2026-09-30: The roadmap is now strategy only, with `status: decided`. It has
  no numbers, no tier lists and no "next" items, and staleness doesn't apply,
  because it changes when strategy changes, not when code does. Rewriting it
  found three more stale claims besides the counts. Its Windows row said the
  build "cannot run anything" and the leg is `experimental: true`, but CI gates
  on six legs, both Windows ones included, all `experimental: false`. It said
  LOD "bought nothing, no decimation", a month after decimation landed. And it
  listed `src/render` as prototype, which is now `working`.
- Nothing was dropped. The R1–R9/A4/A5 history already lives, newer, in
  `renderer-audit-and-plan.md`. The second-asset-root decision is in
  `src/render/shader/info.md`. The two open items became orders: WO-030
  (retire the compiled-in program, including the never-ticked "fps_shooter
  renders identically") and WO-031 (shader hot-reload).
