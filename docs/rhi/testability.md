---
status: decided
covers:
  - src/render/
---
# What GPU-driven costs the test suite

*Original `rhi-design.md` §6. Split out 2026-09-01; wording preserved.*

## 6. The cost nobody mentions: this weakens our quality story

This is on the record because it is the strongest argument *against* the
project and it is specific to this repo.

This engine's trustworthiness rests on two properties:

1. **`rworld::` is GPU-free**, so culling, sort order and LOD selection have tests
   that can fail without a device.
2. **`rdiag::SubmitStats` counts on our side of the API**, because bgfx's Noop
   backend never sets `numDraw` — which is what made `render_pipeline_test`
   possible at all.

**GPU-driven rendering moves both of those onto the GPU.** When the cull is a
compute shader, `render_world_test` no longer tests the cull that ships; when draws
come from an indirect argument buffer the GPU wrote, `SubmitStats` cannot count
them. We would be trading a subsystem with mutation-checked headless tests for one
whose decisions are invisible to the test suite.

That is survivable, but only deliberately:

- **Readback-based assertions.** The compute cull writes its survivor count and
  compacted args to a buffer we read back in a validation mode and compare against
  `rworld::buildVisibleSet` on the same frame. Axiom 5's CPU path is not
  vestigial — it is the oracle.
- **GPU-side asserts** (a debug buffer shaders append to) surfaced through the
  existing `Renderer` log tag.
- **The farm needs real GPUs** — [`method-measurement.md`](method-measurement.md)
  again, from the other direction. Golden-image tests also finally become
  possible, which is the gap keeping `src/render` at `tier: working` today.

Net: the tier for the new path starts at `prototype` and has to earn its way back.
Pretending otherwise is how this becomes a rewrite that is "done" and untrusted.

## The consequence for the tier ladder

Worth stating in the ladder's own vocabulary, because it will look like a
regression in `ENGINE_STATUS.md` and it is not:

> A GPU-driven pipeline enters at `prototype` **by the ladder's own rules**, not
> by an exception. It has no test it can fail on a headless CI box until the
> readback oracle exists, and `working` requires ≥1 real test.

So the readback oracle is not a nice-to-have that follows G6 — it is the
mechanism by which G6 is allowed to count as done at all.
