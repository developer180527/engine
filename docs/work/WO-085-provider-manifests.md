---
status: plan
id: WO-085
title: Provider manifests and platform tables — each provider declared once, built only where it can run
program: providers
priority: P1
size: M
state: done
done: 2026-11-26
evidence: ctest provider_manifest_* (12 configure cases, each refusal by name), providers_test 1b (manifests and registry agree), configure prints built/not built per provider and target
depends: [WO-076]
touches:
  - src/CMakeLists.txt
  - src/plugins/stock_plugins.h
  - src/tools/engine_player.cpp
source: provider-selection.md §3 (DR-0037); research 2026-11-26 (O3DE gem.json platforms are metadata only; CryEngine and O3DE turn off unsupported backends silently)
---
## Why
Today nothing says which platforms a provider supports or what it needs. Jolt, Lua and miniaudio are compiled into every build, and a provider that cannot run on a platform would only be found out when the game runs. The other engines show the trap. O3DE's `gem.json` has a `platforms` list that nothing enforces. CryEngine and O3DE quietly turn off a backend whose SDK is missing, so the build passes. One manifest per provider, read by CMake and by everything that decides, closes both.

## Done when
- [x] a `provider.json` beside each provider (`jolt`, `none` for physics, `lua`, `none` for scripting, `miniaudio`, `none` for audio, `forward-bgfx`, `none` for the renderer) with `kind`, `name`, `contract`, `platforms` (an allow list), `needs`, `sdk`, `requires`, `capabilities` and `differences`; the schema is written down in `docs/reference/`
- [x] one platform table per platform (`cmake/platforms/<name>.json`) with the facts `needs` may name (`display`, `threads`, `dlopen`, `gpu_apis`, …); an unknown fact in a manifest is a configure error
- [x] one CMake call, `engine_provider(<dir> SOURCES …)`, reads the manifest and the target's table: it builds the provider when the platform is listed and every SDK is found, and otherwise records the reason it did not
- [x] the reasons are visible: configure prints one line per provider (built, or not built and why), and the build writes the list where the registry and `--list-providers` read it
- [x] `engine_provider()` refuses a provider target that links another provider's target (R5)
- [x] a manifest the registry does not match (a provider registered under a name no manifest declares, or the reverse) fails a test

## Contract
Nothing changes for a project: the same providers are built on today's platforms, and `project.json` is read as before. The only new failure is a manifest that is malformed or names an unknown platform fact, and that fails at configure time, not in a game.

## Steps
1. Write the six manifests and the desktop platform tables (macOS, Linux, Windows), then Android, iOS and web.
2. `engine_provider()` with CMake's `string(JSON …)`, and the three current providers moved onto it.
3. The "not built, because" list, and the cross-check test against the registry.

## Not in scope
- Choosing per target and refusing at build time: WO-086. This order only declares and builds.
- Loading a provider from a shared library: WO-077. The call builds a static library until then.

## Notes (done 2026-11-26)
- **Providers are not separate libraries yet.** Most are header-only and are compiled into the runtime through the build's list. So `engine_provider()` decides whether each one is built per target, and writes `engine_providers_built.h` with a 1 or 0 per provider. `stock_providers.cpp` registers a provider only when its macro is 1, and won't compile if a macro is missing. Each built provider gets an INTERFACE target that carries its links and includes; Jolt and miniaudio moved off the runtime's own link list onto theirs. WO-077 turns these into real libraries.
- **Flavours were added** (`client`, `server`), because `forward-bgfx` must not be built into the server runtime. The renderer factory's `#if !ENGINE_SERVER_BUILD` became the provider macro, with an `#error` if the two disagree.
- **Platform allow lists are honest:** Jolt, Lua, miniaudio and forward-bgfx list only the three desktops, because those are the only platforms the engine is built and tested on. The `none` providers list all six.
- **CMake wraps long messages**, so the configure-case tests match the text across a line break.
