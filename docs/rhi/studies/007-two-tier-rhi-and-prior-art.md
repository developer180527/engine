---
status: plan
covers:
  - docs/rhi/
---
# Study 007 — Should the RHI be two tiers (WebGPU-shaped, then explicit), and what do wgpu, Dawn, NVRHI, NRI and SDL_GPU teach it?

| | |
|---|---|
| **Status** | `concluded` |
| **Opened** | 2026-10-05 |
| **Concluded** | 2026-10-05 (second pass, WO-056) |
| **Verdict lands in** | `../design-axioms.md` ("Tiers"); `design-api.md`, open decision 3 and `phases.md` link to it (workflow §3: one landing doc) |
| **Highest rung reached** | 3 for the decisive claims (W3C/gpuweb proposals, Khronos and Apple documentation, library headers and READMEs, each opened 2026-10-05 and cited in §6); 5–6 for the shape argument built on them |
| **Superseded by** | — |

## 1. The question

Proposed 2026-10-05: an **explicit** abstraction that only abstracts where the
APIs' *models* differ, built in **two tiers**. First a high tier that also runs
on **WebGPU**, which has no explicit memory management. Then a low tier for
direct graphics programming with no WebGPU in it. Is that the right shape, and
what does the prior art say about each half?

**Why it is expensive to get wrong.** The tier boundary decides what bindless
means (axiom 2). It also decides who owns barriers (axiom 4) and whether the
web is a target at all. Moving that boundary later rewrites every pass.

## 2. The falsifier — written before the work

**The two-tier shape is wrong if** either of these holds:
- the high tier cannot express the engine's GPU-driven path, meaning compute
  cull, indirect draw and per-draw data from buffers (axioms 1 and 5), without
  bindless *on WebGPU*;
- or the low tier ends up re-implementing everything the high tier does, so
  the "high tier" is just a second copy of the code.

**Expected before reading:** the two tiers are right. But "build the high tier
first, from scratch" is probably wrong, because a WebGPU-shaped high tier
already exists as `webgpu.h` (Dawn, wgpu-native).

## 3. Method

Reading only:
- the public headers and design notes of wgpu/wgpu-native, Dawn, NVRHI, NRI,
  SDL3 GPU, Diligent and sokol_gfx;
- the WebGPU spec and its bindless proposal;
- Vulkan descriptor buffers and buffer device address, Metal 4 argument tables
  and residency sets, D3D12 enhanced barriers;
- Sebastian Aaltonen's "No Graphics API" (2025).

This file is the first pass: what I know, each item marked *verify* until its
source has been opened and the section cited in §6.

## 4. What was found

The first pass (2026-10-05, from memory, rung 6) is kept in the git history.
This is the second pass: every claim checked against an opened source, cited
in §6. **Corrections to the first pass are marked ✗.**

### 4.1 The two reads that decide the most

**WebGPU bindless (rung 3, gpuweb `proposals/bindless.md`).** A **Draft**
proposal, created 2025-10-13 (issue #380). It adds `GPUResourceTable` and
WGSL `getResource<T>(index)` / `hasResource<T>(index)`, as **two optional
features** (`"sampling-resource-table"`, `"heterogeneous-resource-table"`),
with a fixed maximum of 65 536 entries. The working group calls it the
priority item for 2026 (meeting notes, 2026-02), and in 2026 it gained a
dependency on a WGSL aliasing extension. Nothing in the proposal states
implementation status in any browser.

**WebGPU indirect count (rung 3, gpuweb #5175, #1354; Chrome 131 notes).**
Multi-draw-indirect exists only as Chromium's non-standard
`"chromium-experimental-multi-draw-indirect"`. The standards issue (#5175,
opened 2025-04-25, Milestone 3, open) proposes `drawCount` and
`maxDrawIndirectCount` and **explicitly excludes** the count-buffer variant
(`vkCmdDrawIndexedIndirectCount`); `drawIndirectCount` itself (#1354) was
deferred to after v1.

**What we conclude:** both things axioms 2 and 5 need are, in WebGPU, either
a draft optional feature or non-standard, and the count buffer is not even
proposed. Tier H stays "portable, not fast" for the foreseeable future. *(Rung
6 on the timeline: no source gives a date.)*

**Dawn versus wgpu-native (rung 3, `webgpu-native/webgpu-headers` README;
wgpu-native README).** `webgpu.h` is the "stable C header". **Dawn and
Emdawnwebgpu implement the stable version; the README states that
"wgpu-native does not yet implement the stable version of this header".**
wgpu-native builds with Rust (MSRV 1.87, MSRV bumps are breaking) and ships
prebuilt 32/64-bit macOS, Windows and Linux releases. Dawn builds with CMake
(`DAWN_FETCH_DEPENDENCIES`, `add_subdirectory`), and Emdawnwebgpu is the same
`webgpu.h` over the browser's JavaScript API (Chrome's "Build an app with
WebGPU"). **Binary size: unverified.** A search snippet quoted 7.6 MB for a
macOS Dawn dylib, but the cited page shows no size; it is not used here.

**What we conclude:** if tier H is built, it is **Dawn**. The deciding fact is
the stable C ABI, not size: an engine that promises a stable ABI to kits
cannot sit on a header its implementation has not adopted.

### 4.2 The libraries, checked

| Library | What the source says (rung 3) | Corrections to the first pass |
|---|---|---|
| **wgpu** | Binding arrays are native-only features: `TEXTURE_BINDING_ARRAY`, `BUFFER_BINDING_ARRAY`, `STORAGE_RESOURCE_BINDING_ARRAY`, `SAMPLED_TEXTURE_AND_STORAGE_BUFFER_ARRAY_NON_UNIFORM_INDEXING`, `PARTIALLY_BOUND_BINDING_ARRAY`; `MULTI_DRAW_INDIRECT_COUNT` is native-only too (`wgpu::Features`) | Naming confirmed. And wgpu-native lags the stable `webgpu.h` (§4.1) |
| **Dawn** | Implements the stable `webgpu.h`; Tint compiles WGSL; Emdawnwebgpu targets the web | — |
| **NVRHI** | Automatic state tracking, switched off with `setEnableAutomaticBarriers`; per-resource alternatives: `keepInitialState`, permanent states; **binding sets AND descriptor tables (bindless)**; backends D3D11, D3D12, Vulkan and **`nvrhi::validation::createDevice`** (validation as a wrapping device); no Metal (Programming Guide). Shaders are **bytes**: `createShader(const ShaderDesc&, const void* binary, size_t binarySize)` (`nvrhi.h`) | — |
| **NRI** | D3D12 (Enhanced Barriers), D3D11, Vulkan 1.2+, **Metal through MoltenVK**, **WebGPU through wgpu-native**, a dummy backend; a C/C++ API of **function tables**; core `NRI.h` plus extension headers (`NRIHelper`, `NRIStreamer`, `NRIRayTracing`, `NRIMeshShader`, …); descriptor pools/sets **and directly indexed descriptor heaps** (`NRIDescriptorHeap.h`); automatic barriers are a non-goal (README) | ✗ First pass said Metal "in progress": it is MoltenVK, not native. ✗ It missed the WebGPU backend and the descriptor-heap path |
| **SDL3 GPU** | "A Note On Cycling": writable resources act as ring buffers, so a write with `cycle` set never touches data a pending command buffer reads. Plain indirect draws exist; no indirect count, no bindless (`SDL_gpu.h`) | ✗ First pass said "no indirect": it has plain indirect, not count |
| **Diligent** | D3D11, D3D12, OpenGL/GLES, Vulkan, **Metal**, **WebGPU**; automatic *or* explicit state transitions; bindless via dynamic resource indexing (README) | WebGPU confirmed |
| **Aaltonen, "No Graphics API"** (blog, 2025-12-16) | GPU pointers for all data; one descriptor heap, textures as 32-bit indices; one 64-bit root pointer instead of a binding API; stage-only barriers (no per-resource tracking, given coherent caches); minimal PSOs. Assumes ReBAR/UMA, coherent last-level caches, per-lane bindless sampling, 64-bit shader pointers | — |

### 4.3 The explicit APIs, checked

- **Vulkan descriptor heap (rung 3, Khronos refpage and blog, 2026-01-23).**
  `VK_EXT_descriptor_heap` shipped in Vulkan 1.4.340: ratified, it
  **removes descriptor sets and pipeline layouts**, descriptors are found by
  heap offset, and "push data" replaces push constants; it requires buffer
  device address. **It is not in the Roadmap 2026 profile**: Khronos seeks
  feedback before a KHR version for a later milestone. ✗ First pass had it as
  "if it has shipped": it has, as an EXT.
- **Metal 4 (rung 3, WWDC25 "Discover Metal 4").** `MTL4ArgumentTable`
  ("in the bindless case, the argument table just needs one buffer binding"),
  residency sets populated at startup and attached to a `MTL4CommandQueue`,
  `MTL4CommandAllocator`-owned command memory, and an explicit stage-to-stage
  barrier API. Hardware: **Apple M1 and later, A14 Bionic and later.**

### 4.4 What this means for the proposal (our conclusions, rung 6 on rung 3)

1. **Abstracting only where the models differ: confirmed.** Vulkan with
   descriptor indexing (or the descriptor heap) and Metal 4 agree on the
   concepts axioms 1–4 need: device addresses, an index-addressed resource
   table, explicit barriers, allocator-owned command memory.
2. **WebGPU cannot be under the bindless tier: confirmed and sharper.** Both
   bindless and the count buffer are absent from standard WebGPU (§4.1).
3. **Tier H, if built, is Dawn's `webgpu.h`**, adopted not written, for the
   stable C ABI (§4.1).
4. **Tier L's descriptor path has two levels.** The floor is
   `descriptor_indexing` (core since 1.2); `VK_EXT_descriptor_heap` is the
   better match for axiom 2 where present, since it removes the descriptor-set
   model axiom 2 already refuses. The handle-is-the-index rule hides which
   one a device uses.
5. **Tier L first: confirmed**, for bindless and GPU-driven reach (DR-0012),
   not R20's old number.

## 5. Verdict

**Two tiers: tier L (ours: explicit, bindless, GPU-driven, Metal 4 and
Vulkan 1.3) built first; tier H (portable) a reach target, adopted as Dawn's
`webgpu.h` if built, never bindless.**

- **Did the falsifier fire?** Condition one fired: a tier on WebGPU cannot
  carry bindless or a count buffer (rung 3, §4.1), so the question was
  re-scoped from "simple, then advanced" to "portable, then fast", and this
  verdict is about the re-scoped question. Condition two did not fire: the
  tiers share passes (the render graph), not code.
- **What changed:** `design-axioms.md` "Tiers" is no longer provisional, and
  names Dawn and the tier-L floors. `design-api.md`, open decision 3 and
  `phases.md` link to it.
- **Still unknown:** any date for WebGPU bindless reaching a stable spec;
  Dawn's shipped binary size (no verified number); and whether the
  descriptor heap reaches KHR. None of them changes the verdict.

## 6. Sources (opened 2026-10-05)

- gpuweb, `proposals/bindless.md` (status, "Resource tables creation", features, limits), and issue #380.
- gpuweb issue #5175 "MultiDrawIndirect feature and maxDrawIndirectCount limit"; issue #1354 "DrawIndirectCount"; Chrome for Developers, "What's New in WebGPU (Chrome 131)".
- GPU for the Web WG wiki, meeting notes 2026-01-07 and 2026-02-24/25.
- `webgpu-native/webgpu-headers`, README (implementations, "stable C header").
- `gfx-rs/wgpu-native`, README (bindings, MSRV, releases).
- Chrome for Developers, "Build an app with WebGPU" ("Get Dawn", "Update CMake settings").
- docs.rs, `wgpu::Features` (binding-array and indirect-count features).
- NVIDIA-RTX/NVRHI, `doc/ProgrammingGuide.md` (state tracking, binding sets, descriptor tables, backends); `include/nvrhi/nvrhi.h` line 3832 (`createShader`).
- NVIDIA-RTX/NRI, README (backends, C interface, extensions, descriptor heaps, barriers).
- libsdl-org/SDL, `include/SDL3/SDL_gpu.h` ("A Note On Cycling"; indirect draw functions).
- DiligentGraphics/DiligentEngine, README (platform and backend table, features).
- Sebastian Aaltonen, "No Graphics API", sebastianaaltonen.com, 2025-12-16.
- Khronos, `VK_EXT_descriptor_heap` reference page; Khronos blog "Vulkan Introduces Roadmap 2026 and New Descriptor Heap Extension", 2026-01-23.
- Apple, WWDC25 session 205 "Discover Metal 4" (argument tables, residency sets, command allocators, barriers, hardware).
