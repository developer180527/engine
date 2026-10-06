---
status: plan
covers:
  - src/render/
---
# Decisions needed before any code

*Original `rhi-design.md` §11. Split out 2026-09-04; wording and numbering
preserved — other documents cite "§11.3".*

> **This file is the study phase's exit condition.** By
> [`workflow.md`](workflow.md) §5, the reading ends when nothing here is
> answerable by reading — every remaining item is decided, or blocked on hardware.
> Answered decisions stay, struck through, with what answered them.

## 11. The list

*Revised 2026-08-28. Decisions 1 and 2 were answered by facts that arrived after
this document was written, not by argument.*

1. ~~**Which two backends is the API designed against?**~~ **Answered: Metal 4 and
   Vulkan 1.3, and there is no third for now.** See
   [`design-axioms.md`](design-axioms.md) axiom 6. D3D12's trigger is Xbox, or a
   Windows Vulkan driver problem that actually shows up rather than one we
   anticipated.
2. ~~**Metal's status** — dev-only?~~ **Answered: no.** Metal 4 is a first-class
   shipping backend and one of the two the API is designed against. See
   [`method-measurement.md`](method-measurement.md) §3.1.
3. **Minimum spec — and this is where two of our own documents contradict each
   other.** *(Raised 2026-08-28.)*

   > `renderer-architecture.md` §1.3: "The low-end floor is real hardware. **Intel
   > UHD 630**, ~128 MB usable VRAM, 4 GB system RAM. **Not a stretch goal — the
   > acceptance test.**"
   >
   > This document, previously: "SM 6.6 / `ResourceDescriptorHeap` and mesh shaders
   > mean roughly **Turing+/RDNA2+**."

   UHD 630 is Gen9.5, 2017. Turing is 2018. **Both cannot be true**, and the
   clustered-forward decision in `renderer-architecture.md` §2 — the reason this
   engine is not deferred — was justified entirely by that 128 MB floor. Nobody had
   written the conflict down.

   It resolves, but only as a TIERED spec rather than one number:

   | capability | UHD 630 (Gen9.5) | needed by |
   |---|---|---|
   | Vulkan 1.3, compute, indirect draw | ✅ | G4–G6 |
   | Descriptor indexing / bindless | ✅ core in 1.2, tighter limits | G6 |
   | Mesh shaders | ❌ | G8 |
   | Hardware ray tracing | ❌ | G7 |

   So **GPU-driven cull → indirect → bindless reaches the stated floor** and the
   floor survives. G7 and G8 do not, and must therefore be explicitly OPTIONAL
   tiers with a working path when absent — not the baseline this document assumed.
   Ray tracing is optional by the same reasoning.

   The iPad floor is a third number, set by argument buffers and
   `MTLIndirectCommandBuffer`, and it has to be stated before G7 rather than
   discovered in it.

   **Answered as tiers (study 007, 2026-10-05):** see
   [`design-axioms.md`](design-axioms.md) "Tiers", which carries the floors
   (not restated here, workflow §3). Still open inside it: whether the Intel
   UHD 630 floor above meets tier L's Vulkan 1.3 floor on its shipped
   drivers, which no source read here settles, and the iPad number below.
4. **G0b hardware** — one NVIDIA + one AMD box on the farm. Every *performance*
   claim after G4 needs them; G1 does not
   ([`method-measurement.md`](method-measurement.md) §3.1).
5. **Do we do incremental extraction first?** It is cheap, independent, and attacks
   the *actual* current bottleneck. Recommendation: yes, in parallel with
   G0–G1, so the frame gets faster while the substrate is being built.
   **Under way:** the retained scene's table landed as P3a (WO-019,
   2026-10-01; `renderer-program.md` §9), with P3b (incremental writes) next.
   The same work re-measured the bottleneck: extraction is 4.0–4.6 ms at
   50 000 real props, not 18.8, so its payoff is now argued on the §9.10 curve
   at 100 k–500 k.
6. **Does the RHI compile shaders?** **Answered: no** (study 002,
   2026-10-05; [`design-api.md`](design-api.md) §4.5). Recommended **no**: it takes bytes
   (DXIL/SPIR-V/metallib) and the cooker stays host-side. That is NVRHI's choice
   and it is what keeps a second consumer from inheriting our content pipeline. It
   also contains [`toolchain-shaders.md`](toolchain-shaders.md), which this
   directory calls the hidden 40%.
7. **Bindless-only, or binding sets?** **Answered: bindless-only** (study
   001, 2026-10-05; axiom 2; validation in [`design-api.md`](design-api.md)
   §4.6). Axiom 2 says bindless-only, and
   GPU-driven at 50 k objects genuinely needs it. Worth recording that every
   reusable RHI shipping today (NVRHI, and NRI's higher-level tier) chose immutable
   binding sets instead, explicitly for validation. Recommend keeping axiom 2 and
   accepting we take the harder validation story — but knowingly.

## What each remaining item is waiting on

| # | Answerable by | Study |
|---|---|---|
| 3 (iPad floor) | device access + Metal docs | 005 |
| 4 | buying hardware | — |
| 5 | under way: P3a landed (WO-019); P3b next | — |
| 6 | answered 2026-10-05 | 002 |
| 7 | answered 2026-10-05 | 001 |

Three of the five need no hardware. Decision 5 is no longer waiting: its first
phase landed, and it measured the bottleneck at a quarter of what this file
assumed.
