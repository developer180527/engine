---
status: plan
id: WO-086
title: The provider resolver — per-target choice in project.json, refused by name at engine_build
program: providers
priority: P1
size: M
state: todo
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
- [ ] `project.json` accepts per-target choices: a plain string for every target, or an object with `default`, platform, flavour (`client`, `server`) and `<platform>.<flavour>` keys, the most specific winning; an unknown key is refused by name
- [ ] one resolver function checks, in order: kind, target keys, name, platform allow list, platform needs, SDK for this configuration, required services, exactly one per kind. Each refusal names the problem and what was available, and each check has a test
- [ ] `engine_build --platform <p> [--flavour <f>]` runs it before configuring the ship tree, and refuses an impossible choice before compiling anything
- [ ] the dev hosts (editor, `engine_host`, a development `engine_player`) run the same function at startup for the machine they are on
- [ ] the shipped player registers its providers from one generated source file that `engine_build` writes. It contains no resolver, manifest reader or provider not chosen, and the symbol check (`player_has_no_cook_stack`, WO-066's successor) says so
- [ ] `dist/providers.lock.json` records each kind's provider, contract version and source revision, and the crash reporter includes it once crash reporting exists
- [ ] `engine providers --platform <p> [--flavour <f>]` (or `engine_build --explain-providers` until the `engine` CLI exists) prints per kind: the choice, the key that chose it, and every other provider with the reason it was not usable; with `--json`
- [ ] the "no display gets `none`" rule from WO-076 becomes the engine's default `server` choice, written in the project's resolved table, not a special case in code

## Contract
A `project.json` with plain strings resolves exactly as today. `engine_build` without `--platform` builds for the machine it runs on, as today. The new failure is an impossible choice, refused at build time with its name.

## Not in scope
- librhi's graphics backend order and device capabilities: WO-089.
- Cross-compiling for a platform the build machine cannot build: `--platform` refuses a target that has no toolchain, by name, and stops there.
