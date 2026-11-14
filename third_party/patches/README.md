# Third-party patches

Local fixes to vendored dependencies. Submodules pin upstream commits, so an
edit made directly in `third_party/<dep>/` lives only in one working tree and
silently disappears on a fresh clone — including in CI. Patches here are
applied on every checkout, so the fix travels with the repo.

## Naming

    <submodule-dir>__<short-description>.patch

The part before `__` is the directory under `third_party/`, which is how CI
knows where to apply it. `bgfx.cmake__shaderc-optimization-genex.patch` applies
inside `third_party/bgfx.cmake`.

## Applying

CI does this automatically (`.github/workflows/ci.yml`). By hand:

    git -C third_party/bgfx.cmake apply --3way \
        "$(pwd)/third_party/patches/bgfx.cmake__shaderc-optimization-genex.patch"

Patches must be applied from inside the submodule: their paths are relative to
that repo's root, and `--3way` needs its object store to resolve context.

## Adding one

Make the change in the submodule, then capture it:

    git -C third_party/<dep> diff <file> > third_party/patches/<dep>__<what>.patch

Verify it exactly describes your working tree:

    git -C third_party/<dep> apply --check --reverse \
        "$(pwd)/third_party/patches/<dep>__<what>.patch"

## Removing one

Every patch here is a divergence from upstream and a maintenance cost — it can
conflict whenever the submodule is bumped. Prefer upstreaming the fix, then
delete the patch once a release contains it.

## Current patches

- **`bgfx.cmake+bgfx__shaderc-shader-language-defines.patch`** — shaderc did not
  define `BGFX_SHADER_LANGUAGE_SPIRV` / `_WGSL` / `_HLSL` / `_DXIL` for their
  profiles, so a shader took the wrong preprocessor branch and glslang tried to
  parse bgfx's dialect as HLSL: *"(75): error at column 19, HLSL parsing
  failed"*. Every shader in the project fails to compile without this. It lived
  as an uncommitted edit inside the nested `bgfx` submodule, so it worked on the
  machine that made it and broke the first fresh checkout — which is exactly the
  divergence this directory exists to prevent. Worth upstreaming (or bumping the
  bgfx pin to a commit that has it), after which this file can go.

- **`bgfx.cmake+bgfx__mtl-null-pointers.patch`** — two member calls on null
  pointers in bgfx's Metal renderer, harmless in practice (messaging nil does
  nothing) but undefined in C++, so UBSan stopped on them the first time a
  sanitizer test rendered through a real Metal device (`eng_window_test`,
  WO-070): `vertexAttributes()->count()` when Metal returns no attribute list
  (GitHub's macOS runners, the virtual "Apple Virtual" GPU), and the render
  encoder at the end of `submit()` when a frame drew nothing (the frame bgfx
  renders inside `init()`, on every device). Worth upstreaming.

- **`bgfx.cmake+bgfx__mtl-timer-query-race.patch`** — bgfx's Metal GPU timer
  writes each frame's begin and end timestamps from Metal's scheduled and
  completed handlers (Metal's thread) and reads them in `TimerQueryMtl::get()`
  (the render thread) with nothing ordering the two: a real data race, found by
  the TSan job when `eng_window_test` first rendered through a real device
  (WO-070). Only bgfx's own `elapsedGpuMs` statistic reads these. The patch
  makes the write a release store and the reads acquire loads. The handlers'
  own invocation is a TSan blind spot, not a race, and is suppressed
  (`tests/tsan_suppressions.cpp`). Worth upstreaming.

- **`bgfx.cmake+bx__shift-amount-in-range.patch`** — bx's 32-bit shift
  helpers mask the shift amount to 0..31. Its branch-free conversions
  (`halfFromFloat` and friends) compute every case and keep one, so a discarded
  case can shift by a negative amount (`halfFromFloat(1.0f)` shifts by -14):
  undefined in C++, found by UBSan in the engine's LUT upload (WO-070). ARM64
  and x86 shift instructions mask the same way, so every selected result is
  unchanged. Worth upstreaming.

- **`bgfx.cmake__shaderc-optimization-genex.patch`** — fixes a malformed CMake
  generator expression in `bgfx_compile_shaders`: `$<IF:$<CONFIG:Debug>:0,3>`
  uses `:` where `$<IF:cond,true,false>` requires a comma, so the shader
  optimization level argument expands to garbage instead of `0` in Debug and
  `3` otherwise. Upstream bug, not a local preference — worth a PR to
  bkaradzic/bgfx.cmake, after which this file can go.
