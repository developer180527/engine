---
status: plan
id: WO-017
title: Cook library split — the runtime never links the cook stack
program: assets
priority: P2
size: L
state: done
done: 2026-10-01
evidence: player_has_no_cook_stack (engine_player, engine_module_probe, server_link_probe: 0 cook-stack symbols; the pre-change player had 19 749 Assimp symbols, and linking the cook stack back in fails it naming 10 parts); engine_player 33.6 -> 24.4 MB in the default build; audit 0 new findings; 127 tests (all but the 10 fuzz explore campaigns)
depends: [WO-011]
touches:
  - src/CMakeLists.txt
  - src/editor/CMakeLists.txt
  - tests/CMakeLists.txt
  - src/runtime/runtime_boot.cpp
  - src/runtime/services/source_import.h
  - scripts/check_no_cook_stack.py
  - cmake/EngineRuntimeConfig.cmake
source: review 2026-09-29 R3 (roadmap "P2 cooker split")
---
## Why
The cookers live in `engine_core`, which links Assimp and the texture encoders `PUBLIC`. So every runtime in the default build, `engine_player` included, carries the whole cook stack. Today the split is a build setting; it should be a library boundary.

## Done when
- [x] an `engine_cook` library (named `engine_cooking`: `engine_cook` is the CLI) holds the cookers, front ends and encoders; `engine_core` links none of them
- [x] the editor links both; `engine_player` and the server link `engine_core` only
- [x] the existing CI symbol check (no Assimp symbols in shipping binaries) also runs on the default-build `engine_player`
- [x] LAYER-03 declares the new edges; the audit baseline doesn't grow

## Log
- 2026-10-01:
  - **Four libraries.**
    - `engine_core`: no cook stack.
    - `engine_cooking` (dev trees): the old `ENGINE_COOKER_SOURCES`, with
      Assimp public and bimg's encoders, the shaderc path and the default
      assets dir private.
    - `engine_runtime`: no importers.
    - `engine_source_import` (dev trees): the AsyncLoader, the runtime glTF
      and Assimp importers, and `sourceimport::install`. These needed both
      halves, the GPU upload in the runtime and the front ends in the cook
      stack, so they could go in neither library alone.

    `engine_cook`, `engine_cook_worker` and `engine_build` link
    `engine_cooking`. The editor, `engine_host` and `scene_resave` link
    `engine_source_import`. The player, module probe and server link a
    runtime only.
  - **The behaviour change, and why it is the point.** `EngineRuntime::init`
    no longer registers source importers or ClipLibrary's source reader. A
    host calls `sourceimport::install(rt)` after init, and the editor,
    `engine_host` and `scene_resave` do. A dev-build `engine_player` used to
    read a `.fbx` scene; now it loads cooked content only, as the shipping
    build always did. WO-018 then deletes the runtime importers.
  - **The symbol check is a ctest, not a `ci.yml` step.**
    `scripts/check_no_cook_stack.py` runs `nm` on the player, the module
    probe and `server_link_probe`, and fails on any symbol of Assimp, the
    cookers, rgbcx, bc7enc or bimg's encoders, naming each. As ctest it runs
    in the default build on every leg; the shipping CI job's Assimp grep
    stays as it was. Where there is no `nm` (MSVC) it says SKIPPED rather
    than passing silently.
  - **LAYER-03:** no module edge changed (the new file is
    `src/runtime/services/source_import.cpp`, whose includes are edges
    `runtime` already had). Libraries are not modules, so the audit baseline
    did not change.
  - **The SDK config** required an `assimp` archive. That was only true while
    `engine_core` linked it publicly, and it meant a shipping install could
    not be found at all. Removed, along with installing the archive and
    replaying its definitions.
  - **Tests** that exercise the cook stack link `engine_cooking` (`LIB` in
    `engine_test` now takes a list). Those that exercise source import link
    `engine_source_import`. A test naming a dev-only library is skipped when
    the tree does not build it.

