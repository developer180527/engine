---
status: plan
covers:
  - docs/rhi/
---
# Study 007 — Should the RHI be two tiers (WebGPU-shaped, then explicit), and what do wgpu, Dawn, NVRHI, NRI and SDL_GPU teach it?

| | |
|---|---|
| **Status** | `in-progress` |
| **Opened** | 2026-10-05 |
| **Concluded** | — |
| **Verdict lands in** | `../design-api.md` |
| **Highest rung reached** | reading, first pass from memory (each §4 claim marked *verify* still needs its source opened, §6) |
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

### 4.1 The libraries

| Library | Level | Binding model | Barriers / memory | What to take | What to avoid |
|---|---|---|---|---|---|
| **wgpu** (Rust; Firefox's WebGPU) | WebGPU-shaped, also native | Bind groups. Native-only `binding_array` features give a bindless subset *(verify current naming)* | Automatic: tracks every resource's state, inserts barriers, owns memory | Validation as a product feature, with errors that name the call. One API from browser to native. `wgpu-native` exposes the standard `webgpu.h` | The per-resource tracking cost on large draw counts is exactly what axiom 4 rejects. No user-controlled memory |
| **Dawn** (Google C++; Chrome's WebGPU) | WebGPU-shaped | Bind groups | Automatic, like wgpu | The reference `webgpu.h`. Its *Tint* compiler turns WGSL into SPIR-V/MSL/HLSL. Mature conformance test suite (CTS) | Large build. Chrome's release cadence |
| **NVRHI** (NVIDIA C++; D3D11/12, Vulkan) | Mid-level | Immutable **binding sets** plus bindless **descriptor tables** | Automatic state tracking, which can be switched off per resource and replaced with explicit barriers | The *opt-out* barrier tracking (the clean form of "explicit where it matters"). Its validation layer as a wrapper device | No Metal. Reference-counted COM-style objects all the way down |
| **NRI** (NVIDIA C; D3D11/12, Vulkan, Metal in progress *(verify)*) | Low-level | Descriptor pools/sets, close to Vulkan | Fully explicit barriers and memory, with optional helper interfaces | C ABI and **interfaces as function tables** that a backend fills in, which suits our kit ABI. A core interface plus optional "helper" and "streamer" extensions | Exposes Vulkan's model nearly unchanged, so Metal is the odd one out |
| **SDL3 GPU** (C; D3D12, Vulkan, Metal) | Mid-level, deliberately small | Per-draw slot binding, no bindless | Automatic within passes, plus "cycling" to avoid read/write hazards | Proof that a *small* C API over the three can ship. Its "cycle" idea for transient buffers | No bindless or indirect-count, so too low a ceiling for us |
| **Diligent** (C++) | Mid-level, many backends including WebGPU *(verify)* | Shader resource bindings plus bindless | Optional automatic transitions | Covers WebGPU and the explicit APIs behind one interface, which is the closest existing answer to your two tiers | Large surface; the "all backends equal" design caps it at the lowest backend |
| **bgfx** (what we have) | High-level | Per-draw uniforms and samplers | Fully hidden | Portability and 15 years of driver quirks | Exactly the axioms 1–2 problems (`evidence-bgfx.md`) |
| **"No Graphics API"** (Aaltonen, 2025) | Thought experiment | GPU pointers plus one descriptor heap; no binding API | Minimal, pipeline-free state where possible | The modern target shape: buffer device address, root pointers, bindless heap. Matches axioms 1–2 almost exactly | It's an essay, not a library |

### 4.2 What this means for the proposal (our conclusions, not the sources')

1. **"Abstract only where the models differ" is right, and NRI is the evidence.**
   Where Vulkan and Metal 4 agree (command buffers, explicit queues, timeline
   semaphores / shared events, buffer device address / `gpuAddress`, argument
   tables / descriptor indexing, residency sets / explicit allocation), expose
   the concept once, thinly. Where they differ (render-pass load/store vs.
   dynamic rendering, residency, heap layout, shader IR), abstract the
   concept, not the call.

2. **WebGPU cannot be under the bindless tier.** Core WebGPU has bind groups
   only. Bindless is a proposal, and multi-draw-indirect-count is an optional
   feature at best *(verify both, 2026)*. So a high tier that runs on WebGPU
   **cannot be "the bindless backend"**: axioms 1–2 hold on the low tier only.
   This is the falsifier's first condition, and it fires **for the web
   backend**, not for the shape.

3. **So the tiers are not "simple, then advanced". They are "portable, then fast".**
   - **Tier H (portable).** The WebGPU *model*: bind groups, automatic
     barriers, no memory control. **Don't write it; adopt `webgpu.h`** through
     Dawn or wgpu-native. That already runs on Metal, Vulkan and D3D12 natively
     and in the browser. Our part is a thin engine layer above it, plus WGSL
     generated by Tint/naga from the same shader source.
   - **Tier L (fast).** Our own explicit, bindless RHI on Metal 4 and Vulkan 1.3,
     with barriers from the render graph (axiom 4) and GPU-driven by default
     (axiom 5). No WebGPU anywhere in it.
   - **The renderer above both** is written against the **render graph**, not
     either tier. Each pass declares what it needs (bindless or binding sets,
     indirect count or a CPU loop), and each tier supplies its own
     implementation. That keeps the falsifier's second condition from firing:
     the tiers share passes, not code paths.

4. **The order should be reversed from the proposal, or run in parallel, not
   "high first".** If tier H is built first and the renderer is ported to it,
   the renderer gets shaped by bind groups and automatic barriers, which is the
   bgfx shape again. Then tier L has to undo it. Two better orders:
   - **(a) Tier L first** on Metal 4 (your machine) and Vulkan. Tier H later,
     as the web and low-end fallback through `webgpu.h`.
   - **(b) Tier H first, but only as an off-the-shelf backend under bgfx's
     replacement, while tier L is designed.** This gets web early, but only if
     the render graph exists first, so passes are tier-neutral from day one.

   The recommendation is (a). Web is a reach target, while the frame-time
   problem (`issues.md` R20) is on the low tier's path.

### 4.3 Making it more modern than the prior art

What none of the above does fully, and this RHI can:
- **Pointers, not descriptors, for buffers.** Buffer device address (Vulkan)
  and `gpuAddress` (Metal) become the per-draw data path. The descriptor heap
  then holds only textures and samplers. This is Aaltonen's shape, and it
  makes axiom 1 nearly free.
- **One descriptor heap with stable 32-bit indices** (axiom 2), using
  `VK_EXT_descriptor_buffer` (or the newer descriptor-heap extension, if it has
  shipped *(verify)*) and Metal 4 argument tables.
- **Barriers only from the render graph** (axiom 4), with NVRHI-style opt-in
  automatic tracking kept for tools and the debug path.
- **A C ABI as function tables, like NRI,** so kits and the vCAD consumer link
  against a stable interface, not C++ classes.
- **Validation as a wrapper device, like NVRHI and wgpu,** compiled in by
  default in dev builds. That answers open decision 7.
- **Shaders as bytes in, one source out** (study 002): Slang or HLSL to SPIR-V
  plus MSL, and WGSL for tier H through Tint/naga.
- **Pipelines as a tiny set:** dynamic state wherever both APIs allow it, so
  the PSO permutation count stays small.

## 5. Verdict

*Provisional, pending the source reads in §6:* **two tiers, yes. Abstracting
only where the models differ, yes. "High tier first" and "a bindless WebGPU
tier", no.** Tier H is `webgpu.h`, adopted and not written, and portable but
not bindless. Tier L is ours, bindless and GPU-driven. The render graph sits
above both, and tier L is built first.

- **Did the falsifier fire?** Condition one fired for the web backend: WebGPU
  cannot carry bindless. That is why the tiers split into "portable vs. fast"
  rather than "simple vs. advanced".
- **What changes:** `design-api.md` gets the tier split; axiom 6 gains "tier H
  via `webgpu.h` for the web and low end"; open decision 3 (minimum spec) is
  answered *as tiers*: H for WebGL2/WebGPU-class devices, L for Metal 4 /
  Vulkan 1.3. Not edited yet: the verdict is provisional.
- **Still unknown:** the bindless-in-WebGPU timeline; whether Dawn or
  wgpu-native is lighter to ship (size, build, C ABI stability); and whether
  G0a shows bgfx is enough for tier H's role, which would make tier H "keep bgfx"
  instead.

## 6. Sources (to open and cite by section before concluding)

- wgpu / wgpu-native: README, `wgpu-types` features (binding arrays), the
  `webgpu.h` header.
- Dawn: `webgpu.h`, the Tint design docs.
- WebGPU spec (W3C), and the gpuweb bindless proposal issue.
- NVRHI: README and the `nvrhi.h` state tracking and binding layout sections.
- NRI: README and `NRI.h` / the `Extensions/` interfaces.
- SDL3: the `SDL_gpu.h` header comments ("cycling").
- Diligent Engine: README backend list.
- Sebastian Aaltonen, "No Graphics API", blog, 2025.
- Vulkan: `VK_EXT_descriptor_buffer`, buffer device address (1.2 core), and
  any descriptor-heap extension. Metal 4: argument tables, residency sets
  (WWDC 2025).
