---
status: target
covers:
  - src/assets/cookers/shader/
---
# The shader toolchain — the hidden 40%

*Original `rhi-design.md` §5. Split out 2026-09-04; wording preserved.*

## 5. The shader toolchain

This is the part that sinks these projects, so it goes in the plan rather than
being discovered in month three. bgfx gives us a shader language *and* `shaderc`
*and* reflection. Replacing it means owning:

- **One source language: Slang** ([DR-0013](../process/decisions/DR-0013-shaders-are-written-in-slang.md),
  study 008, concluded 2026-10-12 by a compile-and-run spike). `slangc` emits
  SPIR-V for Vulkan 1.3 and Metal (MSL, compiled to a metallib by Apple's
  compiler) for Metal 4 directly. `spirv-cross` is kept only as a fallback for
  a shader Slang's Metal target cannot take; none of the spike's did. Metal
  Shader Converter is not on this path (it takes **DXIL**). **DXIL is not a
  target**: D3D12 is deferred (axiom 6), and Slang emits it the day D3D12
  returns. A portable tier H, if one is built (`design-axioms.md` "Tiers"),
  needs WGSL as a **separate, non-bindless variant** of each shader: Slang
  emits WGSL from the same source, but its output for a bindless shader is a
  runtime-sized texture array, which standard WebGPU does not have.
- **The rules the spike found** (Slang 2026.19; re-check them when the pinned
  version changes, `008-spike/spike.sh` does it in one command):
  1. **Bindless is `DescriptorHandle<T>`, stored inside a struct in a buffer**
     (a material, a frame block). On SPIR-V it becomes an index into one runtime
     descriptor array (Slang's resource heap, set 1); on Metal, a texture in an
     argument buffer. Not `Texture2D textures[]`: Slang's Metal output for an
     unbounded array fails Apple's compiler, and a sized one runs out of Metal
     texture slots. Not a bare `StructuredBuffer<DescriptorHandle<T>>`: that
     also fails on Metal.
  2. **`nonuniform(handle)` wherever a handle can differ across a draw's
     pixels** (per-material textures). It is what puts the `NonUniform`
     decoration and capabilities in the SPIR-V; without it a GPU may fetch one
     lane's texture for all of them. `NonUniformResourceIndex` is not the tool:
     Slang's Metal target refuses it on an index, on a handle it is deprecated
     with no effect, and wrapped in a helper function it silently loses the
     decoration. `spike.sh` fails if the lit shader's SPIR-V has none.
  3. **Atomics in buffers are `Atomic<uint>` members**, not `InterlockedAdd` on
     a plain field: WGSL allows atomics only on atomic types, and the typed form
     compiles for all three targets.
  4. **What the RHI must provide for this** (G2): on Vulkan, the runtime
     descriptor array Slang indexes (descriptor indexing, inside the 1.3
     floor); on Metal, writing each texture's GPU resource id into the
     argument buffer and making it resident. These are the bindless heap's two
     implementations, `design-api.md`.
- **Reflection**, though far less of it: bindless means there is almost no binding
  surface left to reflect. We already have `shader_reflect.h`.
- **Variants/permutations**, which we already have machinery for.
- Rewriting every existing `.sc` shader in Slang. There are few (526 lines), which is lucky; the spike's `lit.slang` is `vs_triangle` + `fs_triangle` already ported.

The good news is structural: `shaderc_invoke.cpp` already shells out to an
external compiler with per-profile host gating (a D3D profile is correctly refused
on a macOS host). Swapping the subprocess for `slangc` is a contained change to one
cooker, and the DDC already keys cooked output on cooker version — so the whole
shader cache invalidates itself correctly the day we switch.

## Why this is a separate document

Two reasons, both about scope control.

**It is the named sinkhole.** [`decision-record.md`](decision-record.md) §10 makes
"G3 slips past ~6 weeks" one of three conditions that stop the project. A stop
condition attached to a subsection of a design chapter is easy to forget; one with
its own file is not.

**The RHI must not absorb it.** [`open-decisions.md`](open-decisions.md) decision
6 recommends that the RHI take *bytes* — DXIL, SPIR-V, metallib — and that the
cooker stay host-side. That is NVRHI's choice, and it is what keeps a second
consumer such as vCAD from inheriting our content pipeline. Keeping the toolchain
in its own document is the documentary form of the same boundary.
