---
status: as-built
tier: working
verified: 2026-08-26
covers:
  - src/assets/cookers/shader/
  - modules/assetlib/src/formats/shader_asset.cpp
tests:
  - tests/shader_cooker_test.cpp
parses-external-input: true
---
# Shader cooking

## Purpose
Turn shaders from compile-time `#include`d byte arrays into **cooked assets that
publish a declared interface**, so a material can stop being a fixed C++ struct.

Today [`material.h`](../../../render/material.h) hardcodes five fields
(`baseColorTexture`, `normalMapTexture`, `baseColorFactor`, `roughness`,
`metallic`). "Custom material" therefore means "recompile the engine". Once a
shader declares its own parameters, a material is a shader reference plus
values — and a project can define its own look without touching engine source.
That is Phase 5 of `docs/plans/renderer-audit-and-plan.md`, finding R3.

## The anti-bloat rule
> **Features are a closed list the shader author declares. A material selects a
> combination; it can never author one.**

Unreal's permutation explosion — and the DDC scale, stripping callbacks, PSO
precaching, and static-switch footguns built to survive it — all follow from
letting materials generate shader code. Refusing that one thing keeps the full
variant matrix (`2^features × profiles`) small enough to simply cook all of it,
with no stripping infrastructure at all. `kMaxShaderFeatures = 8` is enforced at
parse time so the rule can't erode quietly.

## Files — one concern each

| File | Concern |
|---|---|
| `shader_manifest.h/.cpp` | Parse + validate a `.shader` (JSON). No compiling. |
| `shaderc_invoke.h/.cpp` | Run `shaderc` for ONE (source, stage, profile, defines). |
| `shader_reflect.h/.cpp` | Read the uniform table back out of compiled bytecode. |
| `shader_verify.h/.cpp` | Does the declaration match what was actually compiled? |
| `shader_cooker.cpp` | `ICooker`: sequence the above, write the container. |
| `assetlib/shader_asset.h` | The `.cshader` container format + save/load. |

## What makes the interface trustworthy
A declared interface is worthless if it can lie. bgfx's shader binary carries
its own uniform table (written at `tools/shaderc/shaderc_metal.cpp:240-260` and
the equivalent in every backend), so the cooker **compiles, reads the truth
back, and rejects a declaration that doesn't match**. Three failures it catches,
all otherwise invisible until someone notices the render looks wrong:

- a parameter naming a uniform the shader doesn't have — the value writes nowhere;
- a parameter whose float offset runs past the uniform's registers — the write
  lands in adjacent uniform memory;
- a sampler declared against a value uniform, or vice versa.

The reverse direction (shader has it, declaration doesn't) is a **warning**:
`u_lights`, `u_viewProj`, `u_shadowMtx` and friends are engine-driven and
legitimately not material parameters. Warnings dedup by name across stages and
across variants — one fact about the shader, reported once.

**Every variant on every profile is verified**, not just the first. That is what
makes the declared interface a contract rather than a convention: a feature may
add engine-driven uniforms (warnings), but it may not remove a declared material
parameter, so conditional material parameters are mechanically impossible.
Backends differ too — a uniform surviving dead-code elimination on one profile
can vanish on another. Verifying only the first variant skipped
`2^n × profiles − 1` of the compiled outputs; when that was fixed it immediately
caught a real bug in our own reflector (see `issues.md`).

## Source invalidation
`.sc` stage sources and the `.sh` headers they include are real inputs but not
registry assets, so `addDependency(UUID)` cannot express them. They are returned
from **`declaredInputs()`** instead, and the PIPELINE hashes each one into the
cook key (`collectSourceTree()` walks `#include "..."` transitively to find
them). Without this, editing shading code looked like "my edit did nothing".

It scans rather than preprocesses, so an include under a false `#if` is still
declared — over-approximating re-cooks unnecessarily, which is the safe
direction.

This used to hash the tree inline into `settingsFingerprint`. Declaring the paths
instead is what makes the omission of an input TESTABLE: `cook_deps_test` perturbs
every declared input of every registered cooker and requires the key to move, so
a cooker added later with an undeclared second input fails a test instead of
silently serving stale output. Ordering no longer matters either — the key sorts
declared inputs, so cross-machine determinism does not rest on the include walk
being stable.

## Parameter packing
Parameters do not each get their own GPU uniform. They pack into a shared `vec4`
at a declared float offset — a hand-declared material constant buffer. bgfx
uniforms are set per-draw, so a dozen tiny uniforms is a dozen `setUniform`
calls per draw; and the existing shaders already pack this way (`u_params.y` is
roughness), so the format describes the shaders **as they are** rather than
requiring a shading rewrite in the same change.

## Cross-compilation: which profiles a host can emit
`shaderc` cannot produce D3D bytecode off Windows — `shaderc_hlsl.cpp:21,91`
guards `D3DCompiler` on `BX_PLATFORM_WINDOWS` (with a Wine path for Linux only),
and `shaderc_dxil.cpp:169-175`'s macOS fallback is literally `"dxcompiler???"`.
`profileCookableOnThisHost()` mirrors those guards and the cook **fails loudly**
rather than producing a package that renders nothing on the target.

Profiles come from the environment, not the asset — targeting is a build
question:

```bash
COOK_SHADER_PROFILES=metal,spirv engine_cook <project> --all
```

Default is everything the host can emit (macOS: metal, spirv, glsl). The profile
set and the shaderc binary's mtime both feed `settingsFingerprint`, so retargeting
or bumping bgfx re-cooks instead of serving a blob that lacks the profile.

## Known limitations
- **Consumed for the standard forward program only.** `ForwardPipeline` loads it
  through `ShaderLibrary` (verified in a shipped dist); the shadow, line and
  skinned programs are still compiled in.
- **`standard.shader` declares zero features.** Skinning is currently a separate
  vertex source (`vs_skinned.sc`) rather than a define; folding it in is the
  natural first real use of the variant matrix.
- **Windows hosts cannot cook shaders at all** — `compileShader` needs a
  `CreateProcess` path. Deferred with the rest of the Windows port.

## Tier evidence (`working`)
- `tests/shader_cooker_test.cpp` — hermetic: manifest validation, reflection
  against hand-built uniform tables, verification rules, and the container
  format including every truncation offset. No shaderc, no GPU.
- End-to-end verified by really cooking `shaders/standard.shader`: 3 profiles
  (metal/spirv/glsl), real `VSH`/`FSH` bytecode, interface round-tripped.
- **Mutation-proved on a real cook**: renaming `u_params` to `u_parms` in the
  manifest fails the cook and names both affected parameters.
- `COOK_SHADER_PROFILES=metal,dx11` on macOS cooks metal and warns that dx11
  needs its own runner.

`parses-external-input: true` — a `.shader` is project-authored JSON and
`shaderc`'s output is an external process's bytes. Reaching `hardened` requires
a FUZZ lane over both the manifest parser and `reflectShaderBinary`.
