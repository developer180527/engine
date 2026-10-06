---
status: target
covers:
  - src/render/
---
# The API, the frame, and what survives

*Original `rhi-design.md` §4.2–4.4. Split out 2026-09-04; wording preserved.*

> `status: target` — this describes intended design. Nothing here exists.

## 4.2 The core API, concretely

```
rhi::Device                 // adapter, queues, feature bits
rhi::Buffer / Texture       // handle IS the bindless index; no views in the API
rhi::Pipeline               // graphics | compute | raytracing, from cooked blobs
rhi::CommandList            // records; never allocates; thread-affine
rhi::TimelineValue          // submit returns one; wait on it, don't fence-juggle
rhi::RenderGraph            // passes, declared resources, derived barriers
rhi::BufferWriter           // ring-allocated upload scratch, frame-scoped
```

Notably **absent**, and deliberately: `setUniform`, `setTexture`, `setState`,
`setTransform`, view IDs. State lives in a pipeline object baked at cook time.
Per-object data lives in buffers. Draw order is the graph plus the sort key we
already build in `rworld::`.

## 4.3 What the frame looks like

```
extract (CPU)      persistent instance buffer, only DIRTY entities re-uploaded
  ↓ copy queue
cull (compute)     frustum + HZB occlusion over the instance buffer
  ↓                → compacted indirect args + per-draw index buffer
draw (indirect)    ExecuteIndirect / vkCmdDrawIndexedIndirectCount, bindless
  ↓
RT passes          inline ray queries for shadows / AO / gameplay visibility
```

The load-bearing consequence: **the CPU stops touching per-visible-object data.**
Extraction becomes an incremental upload of what changed, not a rebuild of
everything — which is the fix for the 18.8 ms that this whole directory started
from, and it is not reachable through bgfx. *(Re-measured 2026-10-01, WO-019, same scene shape: `Render.extract` is now 4.0–4.6 ms. The 18.8 ms here is R20's, kept as the history this was argued from.)*

> **Tiers.** What the API looks like on each tier follows
> [`design-axioms.md`](design-axioms.md) "Tiers" (study 007): the types below
> are tier L's.

That first line is the hardest one and the least designed. "Only DIRTY entities
re-uploaded" is a retained scene with an ownership model, and how shipped engines
solved it — Unreal's proxy plus a single apply point, Unity's `HeapAllocator` and
`SparseUploader` — is [`studies/`](studies/) question 003.

## 4.4 What we keep

More than one might expect, and this is the argument for the migration being
survivable:

- **`rworld::` stays, and stays GPU-free.** Sort keys, LOD selection, light
  packing, frustum math are pure functions over PODs. LOD selection in particular
  is *already* the right shape to run per-instance in a compute shader.
- **The whole asset and cook layer stays.** The DDC, content-addressed cooking,
  the `.cooked` formats, LOD decimation (v5). Only the shader cooker's back end
  changes.
- **`rdiag::SubmitStats` stays and gets more important** (see
  [`testability.md`](testability.md)).
- **`IRenderPipeline` stays** as the A/B seam.

## 4.5 Shaders in: bytes, never source (study 002, 2026-10-05)

`rhi::Pipeline` is created from **compiled bytes**: SPIR-V for Vulkan, a
metallib (or MSL, study 008) for Metal, and cooked WGSL for a tier H. The RHI
never runs a shader compiler; the cooker does, host-side, and the DDC caches
the output. This is NVRHI's interface
(`createShader(const ShaderDesc&, const void* binary, size_t binarySize)`),
and it keeps a second consumer (vCAD) from inheriting our content pipeline.
Answers [`open-decisions.md`](open-decisions.md) decision 6.

## 4.6 Validation: a wrapping device, on by default in dev builds

Bindless-only (axiom 2, kept by study 001) means no API call says which
resources a draw reads, so the API layer cannot check it the way bgfx or
binding sets can. Validation is therefore split by what each side can see:

- **`rhi::ValidationDevice`** wraps any backend device (NVRHI's
  `nvrhi::validation::createDevice` is the model) and checks what the CPU
  knows: every handle's generation on use (a stale index is an error, not a
  read of whatever reused the slot); every index **at the moment it is
  written** into a GPU-visible buffer (in range, LIVE, of the expected
  resource type); that a resource a pass reads is resident and was declared
  to the render graph (axiom 4); timeline values only ever increase; and no
  resource is destroyed while a timeline value that uses it is pending.
- **The backends' own layers** check what only the GPU sees: Vulkan's
  validation layers with **GPU-assisted validation** (out-of-bounds descriptor
  indices, unwritten descriptors, buffer-device-address accesses outside any
  buffer; documented as a significant shader slowdown, so a lane, not the
  default), and Metal's API and shader validation.
- **Defaults:** the validation device is compiled in and ON in dev builds,
  OFF in shipping builds, where it is not compiled at all. GPU-assisted
  validation runs in the Vulkan CI lane (WO-058), not every developer frame.

## 4.7 Backend selection: at runtime (WO-024's requirement)

Today the backend and its shader set are chosen by **compile-time OS checks**
(`src/render/renderer/device.cpp`): anything that is not Apple or Windows gets
Vulkan, whatever the machine can do. The RHI must instead:

- choose the backend **at runtime**, from configuration (a project or command
  line override) plus the device's reported capability;
- look shader blobs up **per backend**, from the cooked output, so one
  distribution carries every backend it supports;
- follow a **named fallback order** and say which one it took: Apple, Metal 4,
  else no tier-L device. Elsewhere, Vulkan 1.3 with `descriptor_indexing`,
  else no tier-L device. In both cases tier H (Dawn) next, if built, else a
  clear startup error naming the missing capability, never a silent
  downgrade.

G2 implements device creation and cites this section ([`phases.md`](phases.md)).

