---
status: decided
id: DR-0013
title: Shaders are written in Slang; SPIR-V and Metal come from one source
date: 2026-10-12
source:
  - docs/rhi/toolchain-shaders.md
---

## Decided
Every shader the RHI and the engine's renderer use is written in Slang and
compiled by `slangc`, pinned to one version (2026.19 when decided), to SPIR-V
for Vulkan 1.3 and to Metal for Metal 4. Bindless resources are
`DescriptorHandle<T>` with `nonuniform()` where a handle can vary, following the
rules in `toolchain-shaders.md`. The RHI takes the compiled bytes (study 002);
the language is the cooker's business, not the RHI's.

## Rejected
HLSL compiled by DXC. It reaches SPIR-V, but Metal needs a second tool
(`spirv-cross`, or Metal Shader Converter, which takes DXIL), and a tier H would
need a third route to WGSL. Also rejected: keeping bgfx's `.sc` dialect, which no
compiler outside bgfx reads.

## Why
Study 008's spike (`docs/rhi/studies/008-spike/`, one command): this engine's
lit, skinned, compute-cull and triangle shaders, ported to Slang, compile to
SPIR-V, Metal and WGSL from one source. Two of them RUN on Metal with results
checked against the CPU, and a broken copy of each fails. Slang's own SPIR-V
validation passes and the lit shader carries the `NonUniform` decorations
bindless needs on Vulkan. Slang labels its Metal target experimental; for these
shaders that label did not bite. The language is governed by Khronos since
2024-11, so it is not one vendor's project.

## What would reverse it
Slang's SPIR-V failing under the Vulkan validation layers on a real driver
(WO-058's lane runs it under lavapipe), or the bindless lit shader failing to
run on Metal once G2 has the argument-buffer path. Either one sends the
affected target to the fallback first (`spirv-cross` for Metal), and only a
failure the fallback cannot fix reopens the language.
