---
status: reference
covers:
  - src/render/
---
# rhi-design.md has moved to `docs/rhi/`

This document was 508 lines and made twelve different kinds of claim — measured
counts, vendor facts, design intent, effort estimates, open questions — under one
`status: plan`. It is now a directory, one purpose per file:
**[`../rhi/`](../rhi/)**, starting at [`../rhi/README.md`](../rhi/README.md).

**This stub exists because twenty-four places in the repo cite this document by
section number.** Rather than edit all of them and risk getting one wrong, the
split kept the original heading numbers inside each new file — `design-axioms.md`
still opens at `4.1`, `phases.md` at `8` — and this table resolves the path.

| you were looking for | it is in |
|---|---|
| §0 the answer up front, §10 what would make me stop | [`../rhi/decision-record.md`](../rhi/decision-record.md) |
| §1 what bgfx costs, measured | [`../rhi/evidence-bgfx.md`](../rhi/evidence-bgfx.md) |
| §2.2 the headroom we never touched | [`../rhi/evidence-bgfx.md`](../rhi/evidence-bgfx.md) |
| §2, §2.1 the contamination counts | [`../rhi/evidence-coupling.md`](../rhi/evidence-coupling.md) |
| §3, §3.1 the measurement trap | [`../rhi/method-measurement.md`](../rhi/method-measurement.md) |
| §4.1 the axioms (*"axiom 2"*, *"axiom 6"*) | [`../rhi/design-axioms.md`](../rhi/design-axioms.md) |
| §4.2–4.4 the API, the frame, what we keep | [`../rhi/design-api.md`](../rhi/design-api.md) |
| §4.5 ray tracing | [`../rhi/design-raytracing.md`](../rhi/design-raytracing.md) |
| §5 the shader toolchain | [`../rhi/toolchain-shaders.md`](../rhi/toolchain-shaders.md) |
| §6 the quality-story cost | [`../rhi/testability.md`](../rhi/testability.md) |
| §7 migration | [`../rhi/migration.md`](../rhi/migration.md) |
| §8 phases (*"G4–G8"*), §9 effort | [`../rhi/phases.md`](../rhi/phases.md) |
| §11 decisions needed (*"§11.3"*) | [`../rhi/open-decisions.md`](../rhi/open-decisions.md) |

New citations should name the file and drop the section number. When the last
`rhi-design.md §N` reference is gone from the repo, delete this file.

Documents still citing it as of 2026-09-04: `renderer-program.md` (16 sites),
`renderer-audit-and-plan.md`, `renderer-vs-production.md`, `platform-embedder.md`,
`resource-policy.md`, `subsystem-audit.md`, `provider-abi.md`.
