---
status: as-built
tier: working
verified: 2026-08-04
covers:
  - docs/
---
# Documentation

Start with **[`../ENGINE_STATUS.md`](../ENGINE_STATUS.md)** — generated from the
tree, it answers *what is true right now* per subsystem. Nothing in it is
hand-maintained, so nothing in it can quietly go stale.

Then **[`process/roadmap.md`](process/roadmap.md)** for what is missing and in
what order it should be built.

## Layout

| Directory | What lives here |
|---|---|
| `architecture/` | How a subsystem is built and **why**. Load-bearing decisions and the invariants that are silent when broken. |
| `guides/` | How to *use* the engine — APIs, scripting, performance tuning. |
| `process/` | How we work: the doc contract, the maturity ladder, the roadmap. |
| `plans/` | Work not yet done. Audits, phased plans, backlog. `plans/future/` is speculative. |
| `generated/` | Doxygen output. Not written by hand, not reviewed. |

Per-subsystem detail lives in `info.md` next to the code, not here — that is
what keeps it honest, since the doc contract compares each one's `verified:`
date against the git history of the code it `covers:`.

## The doc contract
Every doc under `docs/` (except `generated/`) and every `info.md` carries
front-matter: `status`, `tier`, `verified`, `covers`, `tests`, and optionally
`parses-external-input`. `scripts/engine_doctor.py` checks it:

```bash
python3 scripts/engine_doctor.py check     # gate — runs in CI and the docs ctest lane
python3 scripts/engine_doctor.py status    # regenerate ENGINE_STATUS.md
```

A doc goes **stale** when the code it covers changed after its `verified:` date.
For a `hardened` subsystem that is an **error**, not a warning — the tier is a
claim about freshness as much as about tests. See
[`process/engineering-standards.md`](process/engineering-standards.md).

## Where to start reading

- **The asset pipeline** — [`architecture/asset-cook-architecture.md`](architecture/asset-cook-architecture.md).
  §0 is a full end-to-end walkthrough, source file to screen.
- **The renderer** — [`architecture/renderer-architecture.md`](architecture/renderer-architecture.md)
  for the target design, [`plans/renderer-audit-and-plan.md`](plans/renderer-audit-and-plan.md)
  for the ranked findings and which are fixed.
- **Using the engine** — [`guides/engine-api.md`](guides/engine-api.md).
