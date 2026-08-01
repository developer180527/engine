---
status: as-built
tier: working
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
- **Only the standard forward program is cooked.** Shadow, line and skinned
  programs are still compiled in.
- **No hot reload.** A re-cooked shader is not picked up without a restart, even
  though the cook pipeline already watches files.

## How a shader is found — no registry
A shipped dist has **no `registry.db`** (`engine_player` sets
`openAssetDatabase = false`) and resolves everything by paths baked into cooked
content. So the library indexes `<cache>/shaders/*.cooked` by the name each
`.cshader` carries *inside itself*. Identical in dev and dist, no manifest to
keep in sync, and cheap precisely because the closed-feature rule keeps shader
counts small.

The engine's own `standard.shader` reaches a project's `.cache` because
`CookService` scans the engine's shader directory as a **second asset root**
(`ENGINE_DEFAULT_ASSETS_DIR`, `$ENGINE_ASSETS` overrides). Engine defaults are
always in scope, even under `SceneClosure` — no scene references a `.shader`, so
a scoped cook would otherwise produce no programs at all.

## Ordering: the re-attach matters
`openProject()` runs *after* `Renderer::init()`, so the pipeline attaches against
an empty cache and builds the compiled-in fallback. `setShaderCacheRoot()`
re-attaches, mirroring `setShadowResolution()`. Without it the whole path
silently no-ops and every shader edit appears to do nothing.

## Tier evidence (`working`)
- `tests/shader_select_test.cpp` — renderer→profile mapping, exact variant
  matching, both miss diagnoses, program-key identity.
- **Verified in a shipped dist**, not just in the editor: `fps_shooter/dist`
  logs `standard program: cooked .cshader`, with 5 textures resident, physics
  running and gameplay raycasts landing.
- `tests/package_closure_test.cpp` covers the packaging side — a dist that
  contains shader files but not the *name* the pipeline asks for is detectable.

Reaching `hardened` needs an adversarial lane over `.cshader` loading (the blob
is external input, arriving from a shared DDC) and a program-lifetime soak.
