---
status: plan
covers:
  - docs/rhi/
---
# Study 002 — Does the RHI compile shaders, or take bytes?

| | |
|---|---|
| **Status** | `concluded` |
| **Opened** | 2026-09-04 (queued); worked 2026-10-05 |
| **Concluded** | 2026-10-05 (WO-056) |
| **Verdict lands in** | `../design-api.md` ("Shaders in") |
| **Highest rung reached** | 3 (library headers, Apple's tool documentation, WebGPU's `webgpu.h`) |
| **Superseded by** | — |

## 1. The question

Does `rhi::Pipeline` creation take compiled shader **bytes** (SPIR-V,
metallib), or source text it compiles itself?

**Why it is expensive to get wrong.** A compiling RHI drags a compiler (and
our content pipeline) into every consumer, the vCAD host included, and into
every shipped game.

## 2. The falsifier

*This file was written after its reads, which overlapped study 007's second
pass. The expectation is the queue entry's, 2026-09-04.*

**"Bytes" is wrong if** a target backend cannot be fed precompiled bytes, so
the RHI would have to compile at runtime anyway.

**Expected (queue entry):** bytes, NVRHI's choice.

## 3. Method

Reading (rung 3): NVRHI's `nvrhi.h`; Apple's Metal Shader Converter page;
Metal's library loading as used by this engine's cooker today; WebGPU's
shader-module model.

## 4. What was found

**What the sources say:**
- NVRHI takes bytes: `createShader(const ShaderDesc& d, const void* binary,
  size_t binarySize)` and `createShaderLibrary(const void* binary, size_t
  binarySize)` (`nvrhi.h`, line 3832).
- Vulkan consumes SPIR-V words; Metal loads compiled `metallib` libraries
  (Metal Shader Converter's own output is a metallib, from DXIL).
- WebGPU (tier H) takes **WGSL text** and compiles it in the implementation
  (Dawn's Tint). There, "bytes" means the cooked WGSL the cooker produced.

**What we conclude (rung 6):** no tier-L backend needs runtime compilation,
and tier H's compile happens inside Dawn, not in our RHI. The falsifier did
not fire.

## 5. Verdict

**The RHI takes bytes: SPIR-V for Vulkan, metallib (or MSL, see study 008)
for Metal, and cooked WGSL for tier H. It never runs a shader compiler; the
cooker does, host-side.**

- **Did the falsifier fire?** No.
- **What changed:** `design-api.md` "Shaders in" states it; open decision 6
  is answered.
- **Still unknown:** whether Metal pipelines are built from a cooked metallib
  or MSL compiled at load (a cooker choice, study 008), and pipeline caching,
  which is a separate question.

## 6. Sources (opened 2026-10-05)

- NVIDIA-RTX/NVRHI, `include/nvrhi/nvrhi.h`, `IDevice::createShader`, `createShaderLibrary`.
- Apple, "Metal shader converter" (input DXIL, output metallib, runtime requirements).
- `webgpu-native/webgpu-headers` README; Chrome for Developers, "Build an app with WebGPU".
