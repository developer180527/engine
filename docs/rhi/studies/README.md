---
status: reference
covers:
  - docs/rhi/studies/
---
# RHI studies

One file per question. The process is [`../workflow.md`](../workflow.md); the
short version is that a study states its falsifier **before** the work starts,
and its conclusion is propagated to exactly one design document.

Copy [`TEMPLATE.md`](TEMPLATE.md). Number sequentially; never renumber.

## Index

| # | Question | Status | Verdict landed in |
|---|---|---|---|
| — | *none concluded yet* | — | — |

## Queued — questions worth a study, not yet started

Taken from [`../open-decisions.md`](../open-decisions.md) and
[`../phases.md`](../phases.md). The first three are answerable by reading alone;
the rest are blocked on the spike or on hardware.

| # | Question | Why it is expensive to get wrong | Blocked on |
|---|---|---|---|
| 001 | **Bindless-only, or immutable binding sets?** Axiom 2 says bindless-only; NVRHI and NRI's higher tier both chose binding sets, explicitly for validation. | It is the one axiom that cannot be relaxed later — the handle *is* the index, so every shader and every resource type is shaped by the answer. | reading |
| 002 | **Does the RHI compile shaders, or take bytes?** Recommendation is bytes; NVRHI's choice. | Decides whether a second consumer (vCAD) inherits our content pipeline. | reading |
| 003 | **What does the extract→submit sync look like in shipped engines?** Unreal's proxy + single apply point, Unity's `HeapAllocator`/`SparseUploader`, Decima, RAGE. | This is the 18.8 ms the whole project exists to remove; getting the ownership model wrong is a rewrite, not a fix. | reading |
| 004 | **Can bgfx already do GPU-driven cull → indirect draw?** | If yes, G2–G6 lose most of their justification and the project shrinks to bindless. | G0a spike |
| 005 | **The iPad floor** — argument buffers and `MTLIndirectCommandBuffer` limits under a 50 000-part assembly. | vCAD's shipping platform, and the weakest CPU driving the largest object count. | device access |
| 006 | **Is Vulkan-on-Windows good enough to skip D3D12?** Axiom 6 calls this a bet. | The fallback is a third backend — the single largest unschedulable cost in the plan. | G0b hardware |
