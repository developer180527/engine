---
status: decided
verified: 2026-09-30
---
# Roadmap: the order of the big pieces, and what we won't build

Three questions, three documents. Only one of them is written by hand:

| question | where | kept true by |
|---|---|---|
| What is true right now? Tiers, tests, stale docs | [`ENGINE_STATUS.md`](../../ENGINE_STATUS.md) | generated from the tree |
| What do I do next? | [`docs/work/BOARD.md`](../work/BOARD.md), or `python3 scripts/work_orders.py brief` | generated from the work orders |
| **Why that order, and what are we deliberately not building?** | **this file** | a person, when the strategy changes |

**This file holds no numbers, no tier lists and no "next" items.** They are the
parts that went stale: until 2026-09-30 it said 20 subsystems and 6 hardened
while `ENGINE_STATUS.md` said 22 and 9, it called the Windows build unable to
run while CI gated on both Windows legs, and it said LOD had no decimator a
month after one landed. A number copied here is a number that will be wrong.
Status is `decided`: revisit it when the strategy changes, not when code does.

Maturity uses the ladder in [`engineering-standards.md`](engineering-standards.md):
`prototype → working → hardened → production`. Nothing is `production` until a
shipped game has run on it.

---

## 1. The shape of the engine

The **offline half is the strong half.** The cook stack (content-addressed DDC,
thermal governance, memory-budgeted admission, out-of-process crash isolation,
GC) is the most mature part of the engine and is genuinely studio-shaped.

The **online half carries the risk.** The renderer holds the hardest
requirement, 60 FPS on a 128 MB-VRAM integrated GPU, and it is the part a
headless test can see least of.

## 2. The order, and why

**1. Correctness before architecture.** Anything that silently loses the user's
data comes first: a lost authored mesh (WO-029) or a skeleton dropped by the
cook (WO-002). A P0 on the board cannot be parked.

**2. Context before scale.** Work on this engine is intermittent, so a resumable
queue and a one-command re-entry (`brief`) came before any programme that spans
more than one session. See [`docs/work/README.md`](../work/README.md).

**3. Renderer: retained scene before a custom RHI.** An RHI's shape is set by
what its consumer asks of it. Built against today's immediate-mode renderer, it
would be optimised for a consumer we are about to replace. So the retained scene
(P3) lands on bgfx first, and the RHI stays in research (a throwaway spike)
until P3 has said what it needs. Detail and phases:
[`renderer-program.md`](../plans/renderer-program.md).
- *The render graph comes after the need for it.* Two passes do not need a
  graph. Post-processing creates the need, and that is tier-2 customisation,
  worth building only once tier-1 (material + shader) is proven.
- *The GPU backend is chosen at runtime by the RHI, not bolted onto bgfx.*
  Building it on bgfx means building it twice.

**4. Assets: an engine-owned import format before new formats or the cook
split.** Today each source format has a complete cook path of its own, so a new
format is a third copy. `ImportedScene` makes it one front end. The cook-library
split comes after it, because the split depends on what it is splitting.

**5. Portability: make the gaps loud, then close them.** An unknown OS becomes a
compile error with a to-do list before any `os::` layer exists. A port then
fails to build instead of building the wrong thing.

**6. Providers: finish audio's outbound seam before any physics ABI.** Learn
what a provider ABI costs on the smaller subsystem, before freezing a bigger one.

**7. Contracts before independent work.** Every boundary states what a caller
gets before the real implementation exists (null, stub, fake or a pending job),
so each side can be built on a different day. The rule and the registry:
[`docs/contracts/README.md`](../contracts/README.md).

## 3. Known, and deliberately not scheduled

- **Networking.** `modules/net` is an FFI seam with nothing behind it, and the
  transport is undecided. It waits for a game that needs it.
- **Cascaded shadows, occlusion culling, texture streaming.** These are renderer
  phases, sequenced in `renderer-program.md` after the retained scene.
- **The ImGui editor front ends are untested.** The panel logic moved into
  GUI-free models, which are tested, and what is left is drawing.

## 4. Explicitly not being built

- **A material node graph.** Features are a closed list the shader author
  declares. That is the rule that keeps the permutation matrix cookable in full,
  with no stripping infrastructure (`src/assets/cookers/shader/info.md`).
- **PSO baking.** bgfx exposes no PSO API, and every backend already caches
  internally. Revisit with the custom RHI, not before.
- **A fiber job system.** enkiTS is sufficient at this scale.
- **A frozen ABI for third-party renderers.** Swap the backend below the
  renderer, never the renderer through an ABI
  ([`docs/rhi/swappability.md`](../rhi/swappability.md)).

## Where the old content went

- **Renderer findings R1–R9, A4, A5**, with their measurements:
  [`renderer-audit-and-plan.md`](../plans/renderer-audit-and-plan.md).
- **The engine's shaders as a second asset root** (the old §3):
  `src/render/shader/info.md`.
- **The open items:** retiring the compiled-in standard program, which includes
  the unchecked "fps_shooter renders identically", is WO-030. Shader hot-reload
  is WO-031.
