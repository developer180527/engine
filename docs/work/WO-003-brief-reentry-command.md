---
status: plan
id: WO-003
title: One command to regain context — `brief`
program: process
priority: P1
size: S
state: done
done: 2026-09-30
evidence: tests/work_orders_test.py (brief: parsers, scratch tree, real git repo, 2 s budget on the real repo); 5 mutations each red on their own check
depends: [WO-001]
touches:
  - scripts/work_orders.py
source: conversation 2026-09-22 — "I lose context VERY quickly and cannot work daily"
---
## Why
Coming back after a week should take one command, not an afternoon of re-reading.

`work_orders.py next` already answers *what next*. `brief` adds what changed since you last worked, whether the lanes are green, and what is uncommitted. Every line is derived from git or the tree, so none of it can be stale.

## Done when
- [x] `python3 scripts/work_orders.py brief` prints: last commit and its age; commits since a given ref (default: the last commit by you before today); uncommitted file count grouped by top-level area; the `next` output; the stale docs from `ENGINE_STATUS.md`; open questions whose paths overlap the active order's `touches:`
- [x] runs in under 2 s with no build and no network
- [x] `docs/work/README.md` opens with `brief` instead of `next`

## Not in scope
Running the test lanes. `brief` reads results; it never starts builds.

## Log
- 2026-09-30: `brief` runs in ~0.06 s on the real repo. Three choices beyond
  the order's wording:
  - It also prints the **last local test run**, read from ctest's own
    `build/Testing/Temporary/` files: the Why asks "whether the lanes are
    green", and reading a file never starts a build.
  - Open questions match by their `where` trailer **or** by a path named in
    their text. Only 2 of 14 items carry `where`, so the trailer alone would
    almost never fire.
  - With no active order it uses the first ready one, and it still runs when
    an order file is broken, printing the errors in place of the queue:
    that is exactly when you need your bearings.
- 2026-09-30, first real use: the test-run line said "all passed" after a
  5-test docs run, hiding the 3 failures of the previous full run. It now says
  how much ran, with labels, and marks a partial run as PARTIAL. Pinned, and
  its mutation reddens the test.
