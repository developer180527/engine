---
status: reference
covers:
  - src/render/
---
# The RHI programme

The graphics abstraction that replaces bgfx, and the study that decides whether
it should. This directory is the whole record: the standing decision, the
evidence under it, the design, the migration, and — in [`studies/`](studies/) —
the research that is still being done.

> **Nothing here is built.** `src/render/` is bgfx today and stays bgfx until
> [`migration.md`](migration.md) says otherwise. Every file below is a design, a
> measurement, or an open question, and each one says which it is in its
> front-matter `status:`.

## Read in this order

| # | Document | Purpose — the ONE question it answers |
|---|---|---|
| 1 | [`decision-record.md`](decision-record.md) | **Should we replace bgfx at all?** The standing answer, and what would reverse it. |
| 2 | [`evidence-bgfx.md`](evidence-bgfx.md) | **What does bgfx actually cost us?** Measured, plus what bgfx is *not* costing us. |
| 3 | [`evidence-coupling.md`](evidence-coupling.md) | **How much of the engine would have to move?** Counted from the tree. |
| 4 | [`method-measurement.md`](method-measurement.md) | **Which of our numbers are trustworthy?** The SoC trap, and what it does and does not gate. |
| 5 | [`design-axioms.md`](design-axioms.md) | **What does this API refuse to do?** Six refusals, including the backend count. |
| 6 | [`design-api.md`](design-api.md) | **What is the API, concretely?** Types, the frame, and what survives from today. |
| 7 | [`design-raytracing.md`](design-raytracing.md) | **What does ray tracing demand of the resource model?** Designed now, built late. |
| 8 | [`toolchain-shaders.md`](toolchain-shaders.md) | **Who compiles shaders after bgfx?** The hidden 40%. |
| 9 | [`testability.md`](testability.md) | **What does GPU-driven cost the test suite?** The strongest argument against the project. |
| 10 | [`migration.md`](migration.md) | **How do we get there without breaking `main`?** Strangle, never rewrite. |
| 11 | [`phases.md`](phases.md) | **In what order, and what does each phase have to prove?** The `G` sequence. |
| 12 | [`open-decisions.md`](open-decisions.md) | **What is still unanswered?** Live questions only; answered ones move out. |

Plus [`workflow.md`](workflow.md) — how a question becomes a study becomes a
decision, and why studies are not allowed to live in the design documents.

## Where this sits in the renderer programme

[`../plans/renderer-program.md`](../plans/renderer-program.md) decides **order**
across the whole renderer effort and owns the `P` sequence. This directory
decides **content** for the graphics substrate and owns the `G` sequence. The
programme's §4 carries the P↔G mapping and remains the authority on which comes
first.

[`../architecture/renderer-architecture.md`](../architecture/renderer-architecture.md)
is the target renderer *above* this layer — clustered forward, the render graph,
materials. It consumes the RHI; it does not define it.

## Section numbers, and why they were kept

These files were split out of a single 508-line `docs/plans/rhi-design.md` on
2026-09-01. **Twenty-four citations elsewhere in the repo reference that document
by section number** — `§4.5`, `§11.3`, `axiom 2`, `§8 G4–G8`. So the split
preserved the original heading numbers rather than renumbering from 1 in each
file: `design-axioms.md` still opens at `4.1`, `phases.md` at `8`.

That looks odd in isolation and is deliberate. Renumbering would have silently
invalidated every one of those citations — a class of rot with no mechanical
check, since `engine_doctor` reads front-matter and not prose. The old path
`../plans/rhi-design.md` survives as a stub carrying the map below.

| original § | now lives in |
|---|---|
| §0 the answer, §10 what would make me stop | [`decision-record.md`](decision-record.md) |
| §1 measured costs, §2.2 untouched headroom | [`evidence-bgfx.md`](evidence-bgfx.md) |
| §2, §2.1 contamination | [`evidence-coupling.md`](evidence-coupling.md) |
| §3, §3.1 the measurement trap | [`method-measurement.md`](method-measurement.md) |
| §4.1 axioms | [`design-axioms.md`](design-axioms.md) |
| §4.2–4.4 API, frame, what we keep | [`design-api.md`](design-api.md) |
| §4.5 ray tracing | [`design-raytracing.md`](design-raytracing.md) |
| §5 shader toolchain | [`toolchain-shaders.md`](toolchain-shaders.md) |
| §6 the quality-story cost | [`testability.md`](testability.md) |
| §7 migration | [`migration.md`](migration.md) |
| §8 phases, §9 effort | [`phases.md`](phases.md) |
| §11 decisions needed | [`open-decisions.md`](open-decisions.md) |

New citations should use the file path and drop the number.
