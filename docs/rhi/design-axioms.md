---
status: decided
covers:
  - src/render/
---
# The axioms — what this API refuses to do

*Original `rhi-design.md` §4.1. Split out 2026-09-04; wording preserved, including
the axiom numbering that other documents cite ("axiom 2", "axiom 6").*

## 4.1 Axioms

Six, and each one is a thing we refuse rather than a feature we add.

**They describe the fast tier (tier L) only** (see "Tiers" below, from study
007). On a portable tier H, axioms 2, 5 and 6 do not hold, and each says so:

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
   **Kept after study 001 (2026-10-05):** both tier-L APIs now make bindless
   their own model. Metal 4's `MTL4ArgumentTable` needs one buffer binding in
   the bindless case; on Vulkan the floor is `descriptor_indexing` (core
   1.2), and `VK_EXT_descriptor_heap` (1.4.340) removes descriptor sets
   altogether where it is present. The validation this costs is split
   between the RHI's validation device and GPU-assisted validation
   ([`design-api.md`](design-api.md) "Validation").
   *Tier L only:* core WebGPU has bind groups and no bindless, so a portable
   tier H cannot satisfy this axiom (study 007 §4.2).
3. **Explicit queues and timelines.** Graphics, async compute, copy. Compute
   culling and BVH refits overlap graphics; streaming uploads go on copy.
4. **Barriers come from a render graph, never from the caller.** Passes declare
   reads and writes; the graph inserts transitions and aliases transient targets.
   Manual barriers are the #1 source of Vulkan/D3D12 correctness bugs, and fully
   automatic tracking is the thing that made bgfx's model conservative.
5. **GPU-driven is the default path, CPU-driven is the debug path.** Not the other
   way round — otherwise the fast path is the untested one, which is the drift this
   repo already refuses in extraction ("ONE body, serial or parallel").
   *Tier L only:* WebGPU has no guaranteed indirect-count, so on tier H the
   CPU-driven path would be the main path, not the debug one (study 007 §4.2).
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

   *Tier L only:* the two backends are tier L's. A tier H on WebGPU would add
   Dawn or wgpu-native as a third backend DEPENDENCY (adopted, not written), and
   that is still a third set of driver behaviour to test. See "Tiers" below.

## Tiers (study 007, concluded 2026-10-05)

*The verdict of [study 007](studies/007-two-tier-rhi-and-prior-art.md) lands
here, and only here ([`workflow.md`](workflow.md) §3). Its decisive claims are
rung 3: gpuweb proposals and issues, Khronos and Apple documentation, library
headers.*

- **Tier L (fast)** is ours: explicit and bindless, GPU-driven by default.
  All six axioms above are tier L's. **It is built first.** Its floors:
  - **Metal 4:** Apple M1 and later, A14 Bionic and later (WWDC25 "Discover
    Metal 4").
  - **Vulkan 1.3**, with `descriptor_indexing` (core since 1.2) as the
    bindless floor; `VK_EXT_descriptor_heap` used where present. It is an
    EXT outside the Roadmap 2026 profile, so it cannot be required.
- **Tier H (portable)** is the WebGPU model: bind groups, automatic barriers,
  no memory control. If built, it is **Dawn's `webgpu.h`**, adopted not
  written: Dawn implements the stable header, and wgpu-native does not yet.
  It is **never bindless and never indirect-count**: in WebGPU, bindless is a
  Draft optional proposal and multi-draw-indirect is Chromium-only, with the
  count buffer not even proposed. Axioms 2, 5 and 6 do not hold there. It is
  a reach target and is not scheduled ([`phases.md`](phases.md)).
- **The renderer above both is written against the render graph** (axiom 4),
  so passes are tier-neutral. Each pass declares what it needs, and each tier
  supplies its own implementation.

What links here instead of restating it: [`design-api.md`](design-api.md),
[`open-decisions.md`](open-decisions.md) decision 3 (the minimum spec, as
tiers), and [`phases.md`](phases.md) (tier H is not a phase).

## Rungs

By [`workflow.md`](workflow.md) §1, axioms 1–4 rest on rung 3 (vendor
specification) plus rung 1 measurements in
[`evidence-bgfx.md`](evidence-bgfx.md). Axiom 5 is a house rule. **Axiom 6 is
rung 6 — inference — and says so**: it is an explicit bet with a named fallback,
which is the only form a rung-6 decision is allowed to take here.

Axiom 2 is the one axiom that cannot be relaxed later, because the handle *is*
the index; it is [`studies/`](studies/) question 001 and
[`open-decisions.md`](open-decisions.md) decision 7.
