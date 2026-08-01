---
status: as-built
tier: prototype
verified: 2026-08-01
covers:
  - src/render/shader/
tests:
  - tests/shader_select_test.cpp
---
# Shader loading

## Purpose
Turn a cooked `.cshader` into a live bgfx program, so the shaders a game runs
are **content it ships** rather than byte arrays the engine was compiled with.

`ForwardPipeline` still `#include`s `metal/fs_triangle.sc.bin.h` and friends, so
today the only shaders that can ever execute are the ones the engine binary was
built with. That is the concrete mechanism behind finding R3 — `IRenderPipeline`
cannot be a real customization point while the shaders are baked into the
executable.

## Split
| File | Concern | Testable |
|---|---|---|
| `shader_select.h/.cpp` | Which variant does this machine want? | **yes** — GPU-free |
| `shader_library.h/.cpp` | Load it, make the program, cache it. | no — all bgfx |

The split is deliberate: everything with a *decision* in it is in the first
file, so the two failures that are otherwise silent get assertions.

**A package cooked for the wrong backend.** macOS cannot emit D3D bytecode, so a
Mac-cooked package on a Windows/D3D11 machine has no usable variant. The program
never builds, and a renderer with no program draws *nothing* — a black screen
with no error. `selectVariant` fails with the profiles that *were* cooked named
in the message.

**A feature mask that was never cooked.** Selection is an **exact** match, never
nearest. Falling back to a different feature set renders confidently wrong
output: a skinned mesh drawn with the unskinned program collapses into a heap at
the origin, which reads as an animation bug and costs a day to trace.

The two get different messages because the fixes are unrelated — re-cook for the
target, versus fix the material.

## Programs are cached like every other GPU resource
`GpuResourceCache<bgfx::ProgramHandle>`, keyed by
`path # featureMask # profile`. Two materials on the same variant share one
program, refcounted, visible in the VRAM census. All three axes are in the key —
dropping any one means the second material silently draws with the first's
program.

`bgfx::copy`, not `makeRef`, for the bytecode: `makeRef` requires the bytes to
outlive the frame, and the parsed asset is free to move or be dropped.

Uniform handles are deduplicated here too. bgfx refcounts uniforms internally
but hands back a *new* handle per `createUniform` call, so destroying one would
destroy a uniform another material still uses.

## Known limitations
- **Nothing calls this yet.** `ForwardPipeline` still uses the compiled-in
  shaders. Switching it over is gated on a packaging decision — see below.
- **Engine-owned default assets have no home.** The engine's own shaders live in
  `/shaders` (build-time compiled into headers), while cooked assets live in a
  *project's* `.cache`. For the pipeline to load `standard.cshader` there must be
  an answer to "where do the engine's default cooked assets live, and how does
  `engine_build` package them?" That is a real decision, not an implementation
  detail, and it gates finishing Phase 5.
- **No hot reload.** A re-cooked shader is not picked up without a restart, even
  though the cook pipeline already watches files.
- **Shadow / line / skinned programs are still compiled in** — only the standard
  forward program is expressible as a cooked asset today.

## Tier evidence (`prototype`)
`tests/shader_select_test.cpp` covers renderer→profile mapping, exact variant
matching, both miss diagnoses, and program-key identity. That is real coverage
of the decision logic, but the tier stays `prototype` because **no code path in
a running engine executes this yet** — the bgfx half has never created a program
outside a test's imagination. It moves to `working` when `ForwardPipeline`
consumes it and `fps_shooter` renders from cooked shaders.
