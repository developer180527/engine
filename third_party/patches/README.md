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

- **`bgfx.cmake+bgfx__mtl-null-vertex-attributes.patch`** — bgfx's Metal
  renderer calls `vertexAttributes()->count()` on a program's vertex function,
  and on GitHub's macOS runners (the paravirtual "Apple Virtual" device) Metal
  returns NULL for one of bgfx's own programs at init. Messaging nil returns 0
  in practice, so normal builds pass, but a member call on a null pointer is
  undefined in C++ and the UBSan job stopped on it (`renderer_mtl.cpp:3175`)
  the first time a sanitizer test created a real window (`eng_window_test`,
  WO-070). The patch treats NULL as no attributes. Worth upstreaming to
  bkaradzic/bgfx, after which this file can go.

- **`bgfx.cmake+bgfx__mtl-timer-query-race.patch`** — bgfx's Metal GPU timer
  writes each frame's begin and end timestamps from Metal's scheduled and
  completed handlers (Metal's thread) and reads them in `TimerQueryMtl::get()`
  (the render thread) with nothing ordering the two: a real data race, found by
  the TSan job when `eng_window_test` first rendered through a real device
  (WO-070). Only bgfx's own `elapsedGpuMs` statistic reads these. The patch
  makes the write a release store and the reads acquire loads. The handlers'
  own invocation is a TSan blind spot, not a race, and is suppressed
  (`tests/tsan_suppressions.cpp`). Worth upstreaming.

- **`bgfx.cmake__shaderc-optimization-genex.patch`** — fixes a malformed CMake
  generator expression in `bgfx_compile_shaders`: `$<IF:$<CONFIG:Debug>:0,3>`
  uses `:` where `$<IF:cond,true,false>` requires a comma, so the shader
  optimization level argument expands to garbage instead of `0` in Debug and
  `3` otherwise. Upstream bug, not a local preference — worth a PR to
  bkaradzic/bgfx.cmake, after which this file can go.
