---
status: plan
id: WO-056
title: RHI G2 prerequisites — the reading and decisions the first triangle needs
program: renderer
priority: P1
size: M
state: todo
depends: []
touches:
  - docs/rhi/open-decisions.md
  - docs/rhi/studies/README.md
  - docs/rhi/design-api.md
  - docs/rhi/design-axioms.md
  - docs/rhi/toolchain-shaders.md
  - docs/rhi/studies/001-bindless-only-or-binding-sets.md
  - docs/rhi/studies/002-rhi-takes-shader-bytes.md
  - docs/rhi/studies/007-two-tier-rhi-and-prior-art.md
  - docs/rhi/studies/008-shader-source-language.md
source: DR-0012; study 007 (2026-10-05)
---
## Why
G2 writes the API every later phase and every other consumer inherits. Its
shape is still open in writing: study 007 is a first pass with unverified
claims, and open decisions 6 and 7 are unanswered. Writing the backend first
would decide them by accident.

## Done when
- [x] study 007's second pass: every *verify* claim checked against its source, cited by section, verdict landed in `design-axioms.md` "Tiers" (the ONE landing doc, workflow §3; `design-api.md`, open decision 3 and `phases.md` link to it). The two reads that decide the most come first: the WebGPU bindless timeline, and Dawn versus wgpu-native (size, build, `webgpu.h` ABI stability)
- [ ] study 008 concluded: the shader source language (HLSL or Slang) and whether its SPIR-V reaches WGSL for tier H
- [x] study 001 concluded (bindless-only or binding sets), answering open decision 7
- [x] study 002 concluded (the RHI takes shader bytes or compiles), answering open decision 6
- [x] the validation story written: what the wrapper device checks, on by default in dev builds
- [x] WO-024's runtime backend choice is written into `design-api.md` as a requirement
- [x] the minimum spec (open decision 3) answered as tiers, by linking to `design-axioms.md` "Tiers" rather than restating it

## Log
- 2026-10-05, the reading (6 of 7 done):
  - **Study 007 concluded at rung 3.** Every claim from the first pass was
    checked against an opened source, cited by section.
    - The two decisive reads: WebGPU bindless is a Draft optional proposal
      (2025-10-13), multi-draw-indirect is Chromium-only, and the count
      buffer is not even proposed. Dawn implements the stable `webgpu.h`;
      wgpu-native does not yet. So tier H, if built, is Dawn.
    - Corrected: NRI's Metal support is through MoltenVK, and NRI has a
      WebGPU backend; SDL3 GPU has plain indirect draws.
    - Found: `VK_EXT_descriptor_heap` shipped (1.4.340), but outside Roadmap
      2026, so it cannot be required.
    - Tier-L floors from vendor documentation: Metal 4 needs M1 or A14.
    - The verdict landed in `design-axioms.md` "Tiers", no longer
      provisional.
  - **Study 001 concluded: bindless-only, kept.** Both tier-L APIs now make
    bindless their own model; GPU-assisted validation covers what the CPU
    cannot see.
  - **Study 002 concluded: bytes.** NVRHI's `createShader` takes a binary;
    no tier-L backend needs runtime compilation.
  - **Validation** is `design-api.md` §4.6: a wrapping device for CPU-side
    checks (handle generations, indices when written, residency, graph
    declarations, timelines), on in dev builds and absent from shipping;
    GPU-assisted validation in the Vulkan CI lane.
  - **Backend selection** is `design-api.md` §4.7 (and WO-024 is done).
  - **Open decision 3** is answered as tiers by link. Still open inside it:
    the Intel UHD 630 floor against Vulkan 1.3.
  - **Corrected in passing:** Metal Shader Converter takes DXIL, not SPIR-V,
    so `toolchain-shaders.md` no longer lists it on the SPIR-V path.
  - **Not done: study 008.** Reading points to Slang (one source to SPIR-V,
    MSL and WGSL, Khronos-governed since 2024-11-21), but Slang itself
    labels its MSL and WGSL targets experimental. Only a compile spike on
    this engine's shaders (rung 1) can conclude it, and no shader compiler
    is installed here. Downloading Slang needs the owner's go-ahead.

