---
status: as-built
contract: render-pipeline
kind: interface
state: provisional
owner: src/render
header: src/render/render_pipeline.h
implementations:
  - real: src/render/forward_pipeline.h#ForwardPipeline
covers:
  - src/render/render_pipeline.h
verified: 2026-09-27
---

# render-pipeline — the renderer's extension point

`IRenderPipeline`: attach (create programs, uniforms, targets), detach, and
`render(view, ctx)` per view. The forward pipeline is the only implementation;
the renderer owns the output pass that follows every pipeline.

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
Not yet written.
