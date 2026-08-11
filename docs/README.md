---
status: as-built
tier: working
verified: 2026-08-08
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
- **Using the engine** — [`guides/engine-api.md`](guides/engine-api.md),
  [`guides/scripting.md`](guides/scripting.md) and its
  [API reference](guides/scripting-api.md),
  [`guides/performance.md`](guides/performance.md).
- **Replacing a subsystem** — [`guides/audio-provider.md`](guides/audio-provider.md)
  is the worked example: the C ABI a third-party audio engine implements, why it
  has no HRTF/Atmos switches, and the Rust conformance suite that decides
  whether an implementation is one.
- **Is bgfx holding us back?** — [`architecture/renderer-vs-production.md`](architecture/renderer-vs-production.md)
  answers that with numbers (short version: no, and here is what we don't use yet).
- **The rest of the architecture** — [`architecture/overview.md`](architecture/overview.md),
  [`architecture/asset-system.md`](architecture/asset-system.md),
  [`architecture/dependencies.md`](architecture/dependencies.md).
- **Plans not yet started** — [`plans/`](plans/): animation, soak/fuzz testing,
  scripting integration, the infrastructure backlog, and `plans/future-plans/`
  for notes that are deliberately speculative (crash reporting, IDE ideas).

This list is hand-maintained and therefore the one thing here that CAN go stale.
`engine_doctor` checks front-matter, not prose — see the note in
[`process/engineering-standards.md`](process/engineering-standards.md).
