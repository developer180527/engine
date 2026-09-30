---
status: reference
---
# Work orders

**Coming back after a break? Run this first:**

```bash
python3 scripts/work_orders.py brief
```

It prints, in well under a second and without building anything:

- the last commit, and every commit since your last one before today (or
  `--since <ref>`)
- what is uncommitted, grouped by area
- the last local test run: when, and what failed
- the stale docs from `ENGINE_STATUS.md`
- what is in progress, the exact items still open on it, and the next three
  ready orders
- open questions that touch the files the current order will change

`python3 scripts/work_orders.py next` is the queue part alone. The full
picture is [BOARD.md](BOARD.md).

## What this is

Every piece of planned work is one file here: `WO-NNN-short-name.md`. The board
is **generated** from those files, so it cannot go out of date. You never edit
the board. You edit an order, then run `python3 scripts/work_orders.py board`.

Plans in `docs/plans/` say *what we are building and why*. Orders here say
*what to do next, in what order, and how we know it is finished*. When a plan
turns into work, it becomes orders.

## An order

```markdown
---
status: plan                 # always `plan`, which is the doc contract's kind for backlog
id: WO-012
title: cgltf front end (static meshes)
program: assets              # process | assets | portability | renderer | providers
priority: P2                 # P0 broken now · P1 cheap, and makes the rest cheaper · P2 programmes · P3 later
size: M                      # S one session · M 2-4 · L a week or more · XL must be split
state: todo                  # todo | active | done | parked
depends: [WO-011]
contracts: [cooker]          # contracts in docs/contracts/ this order changes
new-contracts: [import-frontend]   # contracts this order creates
touches:                     # files it will change (must exist until it is done)
  - src/assets/cookers/mesh/mesh_cooker.cpp
source: review 2026-09-29 C1 # where the order came from
---
## Why
One paragraph. The first line is shown on the board, so make it count.

## Done when
- [ ] a checkable thing
- [ ] a test that fails without the change

## Contract
Nothing: what a caller gets before the real implementation exists.

## Steps
## Not in scope
```

Finished orders add `done: YYYY-MM-DD` and `evidence:` (a commit, a test, or a
measurement). Parked orders add `parked-until:`, which says what would unpark them.

## The rules the checker enforces

`python3 scripts/work_orders.py check` refuses:

| rule | why |
|---|---|
| **No more than 2 orders active** | Half-finished work is what costs context. Finish or park one before starting another. |
| **Nothing XL unless parked** | If it's too big to start, it's too big to keep in your head. Split it first. |
| **"Done when" must be checkboxes** | "Done" must be checkable. The ticks are also the progress shown on the board, so you can see where you stopped. |
| **Done means every box ticked, plus a date and evidence** | Nothing is done because it *feels* done. |
| **An order that touches a contract needs a `## Contract` section with a `Nothing:` line** | This is the contract rule (below). |
| **Contracts named must exist in `docs/contracts/`** | New ones go under `new-contracts:` until they're registered. |
| **`touches:` paths must exist** | If a file moves, the order says so, the same way `covers:` does for docs. |
| **Dependencies must exist, no cycles, and nothing active or done while one of its dependencies is open** | "Ready" and "blocked" are computed, never claimed. |
| **Parked needs `parked-until:`; a P0 can't be parked** | Parking is a decision you write down, not a way to forget something. |

## The contract rule

This is the rule the whole system exists to enforce. Any work that creates or changes a subsystem boundary
must first answer: **what does a caller get before the real thing exists?**
One of four answers (see `docs/contracts/README.md`):

- **null**: does nothing, correctly, forever. For example, a server has no GPU.
- **stub**: "not done yet", and says so loudly. It logs once and returns a status, so silence is never mistaken for success.
- **fake**: plausible behaviour for tests.
- **job**: the answer isn't ready yet. The caller gets a handle that is *pending* and later becomes *ready* or *failed*, and it must handle all three.

With that answer written down, the code on either side of the boundary can be built on
different days, by different people, in either order. Each side compiles and
runs against the contract, not against the other side.

## Where orders come from

Reviews, audits, plans, bugs. Put the source in `source:` so an order can be
traced back to the finding that caused it.
