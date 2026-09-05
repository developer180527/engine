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
| `reference/` | File formats and schemas, field by field. Lookup material, not narrative. |
| `process/` | How we work: the doc contract, the maturity ladder, the roadmap. Also the defect record — [`bug-ledger.md`](process/bug-ledger.md) is the schema and [`bugs/`](process/bugs/) is one file per defect; [`open-questions.md`](process/open-questions.md) is what is known and *not* fixed; [`port-log-windows.md`](process/port-log-windows.md) is what the cross-platform port cost. |
| `plans/` | Work not yet done. Audits, phased plans, backlog. `plans/future/` is speculative. |
| `rhi/` | The graphics-abstraction programme, one purpose per file — the decision, its evidence, the design, the migration. [`rhi/workflow.md`](rhi/workflow.md) is how a question becomes a study becomes a decision; [`rhi/studies/`](rhi/studies/) is where the research lands. |
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
- **Hosting the engine** — [`plans/platform-embedder.md`](plans/platform-embedder.md):
  the host owns the window, the loop and the surface; the engine is a guest.
  Prerequisite for an iPad shell, a dedicated server, and the RHI's surface
  question alike.
- **How much of the machine to take** — [`plans/resource-policy.md`](plans/resource-policy.md):
  size the engine to the WORK, not to the machine. Thread QoS (measured at
  14.5x under contention), worker counts, frame pacing, and what upscaling
  demands of the renderer before it can exist at all.
- **The renderer** — [`plans/renderer-program.md`](plans/renderer-program.md) is the
  START HERE: what we are building, in what order, and why the reuse boundary sits
  *below* the renderer rather than through it. Then
  [`architecture/renderer-architecture.md`](architecture/renderer-architecture.md)
  for the target design, [`plans/renderer-audit-and-plan.md`](plans/renderer-audit-and-plan.md)
  for the ranked findings and which are fixed, and **[`rhi/`](rhi/)** for the
  GPU-driven RHI itself — a directory now, not a file, starting at
  [`rhi/README.md`](rhi/README.md).
- **Does a swappable renderer cost performance?** —
  [`rhi/swappability.md`](rhi/swappability.md): no, and the measurement is in it.
  "Swappable" means four different things; three are free and the fourth (a frozen
  renderer ABI) is the only irreversible one, so it is the one not being built.
- **How far off AAA are we?** — [`plans/aaa-gap-analysis.md`](plans/aaa-gap-analysis.md):
  eight pillars, an evidence line per claim, and the one gap that is not additive.
  Short version: the expensive half is built, the visible half is not.
- **Using the engine** — [`guides/engine-api.md`](guides/engine-api.md),
  [`guides/scripting.md`](guides/scripting.md) and its
  [API reference](guides/scripting-api.md),
  [`guides/performance.md`](guides/performance.md).
- **Extending the engine** — [`architecture/extension-model.md`](architecture/extension-model.md)
  defines the four ways code attaches: Plugins (static), Kits (dynamic,
  version-stable), Add-ons (out-of-process, crash-proof) and Providers
  (subsystem replacement). Also the ABI rules and why they cost what they cost.
- **What belongs in the editor** — [`architecture/tool-ecosystem.md`](architecture/tool-ecosystem.md)
  answers "inside the editor, or its own program?" with a rule rather than a
  preference: authoring outside, tuning and viewing inside. Also the seat rule
  (everyone has the editor, nobody needs every tool), what orchestration actually
  requires, and the costs of the model stated rather than hidden.
- **What to strengthen next** — [`plans/subsystem-audit.md`](plans/subsystem-audit.md)
  ranks every subsystem by blast radius rather than by how rough it feels, on one
  principle: hardening propagates downward, so a test on a foundation defends
  everything above it. Also the two subsystems that are invisible to
  `ENGINE_STATUS.md`, and what the future systems (crash reporting, fibers, a
  dedicated server) are really waiting on.
- **Writing a scene by hand** — [`reference/scene-format.md`](reference/scene-format.md)
  is the `.scene` schema: every component, every field, every default, and — the
  half that matters most — what a wrong value silently does instead of failing.
  The no-editor path in `tool-ecosystem.md` §4 depends on this format being
  hand-writable, so this is its reference.
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
