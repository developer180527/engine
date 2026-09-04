---
status: decided
covers:
  - src/render/
---
# The axioms — what this API refuses to do

*Original `rhi-design.md` §4.1. Split out 2026-09-04; wording preserved, including
the axiom numbering that other documents cite ("axiom 2", "axiom 6").*

## 4.1 Axioms

Six, and each one is a thing we refuse rather than a feature we add:

1. **No per-draw uniforms. Ever.** All per-draw data lives in GPU buffers indexed
   by draw ID. This single rule deletes the 8 MB ceiling, the `kMaxDrawsPerFrame`
   guard, the one-deep material-bind cache (`R7`), and it is what makes indirect
   draws expressible at all.
2. **Bindless-only.** Every texture and buffer receives a shader-visible 32-bit
   index at creation. There is **no binding API** — `rhi::TextureHandle` *is* the
   index the shader indexes with. D3D12: one `CBV_SRV_UAV` heap with SM 6.6
   `ResourceDescriptorHeap[]`. Vulkan: one giant descriptor set with
   `descriptor_indexing` (core 1.2) and `nonuniformEXT`. This is a *smaller* API
   than bgfx's, not a bigger one.
3. **Explicit queues and timelines.** Graphics, async compute, copy. Compute
   culling and BVH refits overlap graphics; streaming uploads go on copy.
4. **Barriers come from a render graph, never from the caller.** Passes declare
   reads and writes; the graph inserts transitions and aliases transient targets.
   Manual barriers are the #1 source of Vulkan/D3D12 correctness bugs, and fully
   automatic tracking is the thing that made bgfx's model conservative.
5. **GPU-driven is the default path, CPU-driven is the debug path.** Not the other
   way round — otherwise the fast path is the untested one, which is the drift this
   repo already refuses in extraction ("ONE body, serial or parallel").
6. **TWO backends: Metal 4 and Vulkan 1.3. D3D12 is deferred, and Xbox is its
   trigger.** *(Rewritten twice on 2026-08-28. The original read "three backends,
   only two of them ship", with Metal 3 as a dev-only backend "explicitly allowed
   to be slower and feature-reduced". The first correction made all three ship.
   This one cuts the count to two.)*

   **Coverage is why.** Between them these two reach every platform either product
   ships on, and D3D12 adds exactly one thing neither covers:

   | | Metal 4 | Vulkan 1.3 | D3D12 |
   |---|---|---|---|
   | macOS, iPadOS, iOS, visionOS | ✅ | MoltenVK only | — |
   | Windows | — | ✅ | ✅ |
   | Linux, Steam Deck / Proton | — | ✅ | — |
   | Android | — | ✅ | — |
   | **Xbox** | — | — | **✅ only** |

   **Complexity is the decisive argument, not coverage.** [`phases.md`](phases.md)
   §9 already names the dominant unschedulable risk of this whole project: the ~15
   years of driver quirks bgfx absorbs for us, which we rediscover one vendor at a
   time. A third backend multiplies that risk, the validation setups and the CI
   legs — forever, for one developer, to reach a platform neither product ships on
   today.

   **Metal 4 is what makes two backends sufficient rather than a compromise.**
   The earlier argument for pairing Metal with D3D12 was that they disagree most,
   so an abstraction satisfying both is unlikely to be secretly shaped like
   either. That was reasoning about **Metal 3**, which tracked resources
   implicitly. Metal 4 is an explicit API: resources are **untracked by default**
   and need explicit barriers, `MTL4ArgumentTable` replaces per-resource binding
   (this is the bindless model of axiom 2), residency sets make resources resident
   with minimal per-frame CPU cost, and `MTL4CommandBuffer` is reusable via
   `beginCommandBuffer(allocator:)`. That is structurally Vulkan's shape. The
   axioms above translate to both without either being bent.

   Enough divergence remains — residency sets versus memory heaps, argument tables
   versus descriptor sets, the queue and submission models — that an abstraction
   satisfying both is unlikely to be a thin veneer over one of them. That is the
   property that keeps D3D12 *later a backend rather than a redesign*.

   **The bet, stated so it is a decision:** Vulkan-on-Windows is not D3D12, which
   is Windows' native and generally best-tested path, especially on Intel iGPUs.
   It is a good bet — every IHV ships Vulkan on Windows and Proton has hardened it
   enormously — but it is a bet, and the fallback is adding the third backend.

## Rungs

By [`workflow.md`](workflow.md) §1, axioms 1–4 rest on rung 3 (vendor
specification) plus rung 1 measurements in
[`evidence-bgfx.md`](evidence-bgfx.md). Axiom 5 is a house rule. **Axiom 6 is
rung 6 — inference — and says so**: it is an explicit bet with a named fallback,
which is the only form a rung-6 decision is allowed to take here.

Axiom 2 is the one axiom that cannot be relaxed later, because the handle *is*
the index; it is [`studies/`](studies/) question 001 and
[`open-decisions.md`](open-decisions.md) decision 7.
