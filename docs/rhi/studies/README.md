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
| 001 | [Bindless-only, or immutable binding sets?](001-bindless-only-or-binding-sets.md) | concluded 2026-10-05: bindless-only (rung 3) | `../design-axioms.md` (axiom 2) |
| 002 | [Does the RHI compile shaders, or take bytes?](002-rhi-takes-shader-bytes.md) | concluded 2026-10-05: bytes (rung 3) | `../design-api.md` (§4.5) |
| 007 | [Two tiers (WebGPU-shaped, then explicit), and what wgpu, Dawn, NVRHI, NRI and SDL_GPU teach](007-two-tier-rhi-and-prior-art.md) | concluded 2026-10-05: tier L first; tier H = Dawn's `webgpu.h`, never bindless (rung 3) | `../design-axioms.md` ("Tiers") |
| 008 | [The shader source language: HLSL (DXC) or Slang, and the WGSL path](008-shader-source-language.md) | in-progress: Slang proposed (rung 3); needs a compile spike (rung 1) to conclude | `../toolchain-shaders.md` |

## Queued — questions worth a study, not yet started

Taken from [`../open-decisions.md`](../open-decisions.md) and
[`../phases.md`](../phases.md). Of those left, 003 is answerable by reading alone;
the rest are blocked on the spike or on hardware.

| # | Question | Why it is expensive to get wrong | Blocked on |
|---|---|---|---|
| 003 | **What does the extract→submit sync look like in shipped engines?** Unreal's proxy + single apply point, Unity's `HeapAllocator`/`SparseUploader`, Decima, RAGE. | The ownership model of the retained scene (P3) and of G6; getting it wrong is a rewrite, not a fix. (This row first said "the 18.8 ms the whole project exists to remove"; WO-019 re-measured that extraction at 4.0–4.6 ms.) | reading |
| 004 | **Can bgfx already do GPU-driven cull → indirect draw?** | If yes, G2–G6 lose most of their justification and the project shrinks to bindless. | G0a spike |
| 005 | **The iPad floor** — argument buffers and `MTLIndirectCommandBuffer` limits under a 50 000-part assembly. | vCAD's shipping platform, and the weakest CPU driving the largest object count. | device access |
| 006 | **Is Vulkan-on-Windows good enough to skip D3D12?** Axiom 6 calls this a bet. | The fallback is a third backend — the single largest unschedulable cost in the plan. | G0b hardware |
