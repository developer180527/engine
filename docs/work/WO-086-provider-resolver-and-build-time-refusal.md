---
status: plan
id: WO-086
title: The provider resolver — per-target choice in project.json, refused by name at engine_build
program: providers
priority: P1
size: M
state: done
done: 2026-11-26
evidence: tests/provider_resolve_test.cpp (one case per check, web and server targets), providers_test 3b, ctest engine_build_* (explain, refusal for web, a web project, an unknown platform), a shipping tree built with a fixed selection: --list-providers showed only the four chosen, nm found no provsel, Jolt or miniaudio symbols
depends: [WO-085]
touches:
  - src/tools/engine_build.cpp
  - src/project/project_context.h
  - src/plugins/stock_plugins.h
  - src/tools/engine_player.cpp
source: provider-selection.md §4–5 (DR-0037); research 2026-11-26 (Unreal, Godot, O3DE and CryEngine all find a bad provider choice when the game runs)
---
## Why
`engine_build` packages a game for the machine it runs on, and nothing checks the chosen providers against the target before the game starts. A project cannot choose a different renderer for the web, or no renderer for a dedicated server. Every engine studied finds this kind of mistake late: a dummy fallback, an ignored flag, or a fatal error at startup. One resolver, run before anything compiles, turns it into a refusal at build time that names the problem and what was available.

## Done when
- [x] `project.json` accepts per-target choices: a plain string for every target, or an object with `default`, platform, flavour (`client`, `server`) and `<platform>.<flavour>` keys, the most specific winning; an unknown key is refused by name
- [x] one resolver function checks, in order: kind, target keys, name, platform allow list, platform needs, SDK for this configuration, required services, exactly one per kind. Each refusal names the problem and what was available, and each check has a test
- [x] `engine_build --platform <p> [--flavour <f>]` runs it before configuring the ship tree, and refuses an impossible choice before compiling anything
- [x] the dev hosts (editor, `engine_host`, a development `engine_player`) run the same function at startup for the machine they are on
- [x] the shipped player registers only the chosen providers, from the header CMake generates for `engine_build`'s selection (`ENGINE_PROVIDERS_SELECTED`). It contains no resolver, manifest reader or provider not chosen. Checked by hand with `nm` on a shipping tree; the automated symbol check is WO-088's
- [x] `dist/providers.lock.json` records each kind's provider, contract version and source revision, and the crash reporter includes it once crash reporting exists
- [x] `engine providers --platform <p> [--flavour <f>]` (or `engine_build --explain-providers` until the `engine` CLI exists) prints per kind: the choice, the key that chose it, and every other provider with the reason it was not usable; with `--json`
- [x] the "no display gets `none`" rule from WO-076 becomes the engine's default `server` choice, written in the project's resolved table, not a special case in code

## Contract
A `project.json` with plain strings resolves exactly as today. `engine_build` without `--platform` builds for the machine it runs on, as today. The new failure is an impossible choice, refused at build time with its name.

## Not in scope
- librhi's graphics backend order and device capabilities: WO-089.
- Cross-compiling for a platform the build machine cannot build: `--platform` refuses a target that has no toolchain, by name, and stops there.

## Notes (done 2026-11-26)
- **The generated file is CMake's header, not a separate source file.** `engine_build` passes the choice as `-DENGINE_PROVIDERS_SELECTED=physics=jolt;…`. `EngineProviders.cmake` then builds only those providers and writes `ENGINE_PROVIDERS_FIXED` and `ENGINE_PROVIDERS_CHOSEN_<KIND>`. The existing build list registers what is built, so no second list was needed.
- **Two checks, both before compiling.** The resolver runs from the source tree's manifests before cooking. After the shipping tree is configured, `build-ship/providers.json` is read, and a chosen provider that CMake could not build (for example, its SDK is missing) is refused before the player compiles.
- **A runtime with no display resolves as `server`**, and its platform's `display` fact is false. The old "no display gets `none`" special case is gone. The engine's default is `"renderer": {"default": "forward-bgfx", "server": "none"}`. A project that writes a plain `"renderer": "forward-bgfx"` is refused when it runs headless.
- **Resolving needs the platform started** (`supportsRendering()` is only true once the window exists). That moved the check after the job pool and profiler channels are set up, which exposed BUG-0076: a failed `init()` never undid them. `abortInit()` now does.
- **`--platform` other than this machine's** resolves and explains, then refuses to package ("no toolchain for it here"). **`--flavour server`** does the same, because there is no server player to package yet.
- **Not run end to end:** a full `engine_build` package (cook, kits, assemble) with the new step. The pieces were tested separately: the step itself by ctest, and the shipping tree build by hand.
