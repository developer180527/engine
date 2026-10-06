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

- **One source language, HLSL 2021 / SM 6.6+ for now, and that choice is
  OPEN** (study 008: HLSL through DXC, or Slang). Either way: SPIR-V for
  Vulkan 1.3, and SPIR-V → MSL via `spirv-cross` for Metal 4. (This once said
  "or Metal Shader Converter": MSC takes **DXIL**, not SPIR-V, so it is only an
  option on an HLSL → DXC → DXIL route; study 008.) Metal 4 is a full
  shipping backend since axiom 6's rewrite, not a "dev backend" as this line
  once said. **DXIL is not a target**: D3D12 is deferred (axiom 6), and DXIL
  returns with it. A portable tier H, if one is built (`design-axioms.md`
  "Tiers"), needs WGSL, and that is a **separate, non-bindless variant** of
  each shader, not a conversion of tier L's: Tint's SPIR-V reader does not
  take runtime-indexed descriptor arrays. One source that emits both variants
  is the case for Slang (study 008, proposed, pending a compile spike).
- **Reflection**, though far less of it: bindless means there is almost no binding
  surface left to reflect. We already have `shader_reflect.h`.
- **Variants/permutations**, which we already have machinery for.
- Rewriting every existing `.sc` shader in HLSL. There are few, which is lucky.

The good news is structural: `shaderc_invoke.cpp` already shells out to an
external compiler with per-profile host gating (a D3D profile is correctly refused
on a macOS host). Swapping the subprocess for DXC is a contained change to one
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
