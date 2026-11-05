---
status: plan
id: WO-066
title: Shipping build — development machinery absent from the binary, not switched off
program: process
priority: P2
size: L
state: todo
depends: []
touches:
  - src/tools/engine_build.cpp
  - src/tools/engine_player.cpp
  - src/CMakeLists.txt
  - tests/CMakeLists.txt
  - src/core/logger.h
source: review 2026-11-06 (how engine_build strips a game); engine-architecture.md §2a
---
## Why
`engine_build` already compiles the cook stack out of the player (`ENGINE_WITH_SOURCE_IMPORTERS=OFF`, checked by `player_has_no_cook_stack`). Other development machinery is still compiled into the shipped game, just switched off at runtime. Hot reload is `Reload::Never`, engine log chatter is muted by `quietForShipping()`, and the profiler and stats channels are compiled in. Files are packaged by extension, not by what the game uses. Every `.lua` and audio file under `assets/` ships, and Lua ships as source. Nothing checks the exported folder. `engine-architecture.md` §2a says a shipped game has none of this: not disabled, absent, because a flag can be flipped and absent code cannot run.

## Done when
- [ ] one build setting, `ENGINE_SHIPPING`, set by `engine_build` for the player it builds, alongside `ENGINE_WITH_SOURCE_IMPORTERS=OFF`
- [ ] under it the profiler and stats channels (`frame_stats_channel`, `mem_channel`, `input_latency_channel`), hot reload, and engine-category logging are compiled out; first, list what is actually live in a shipped player today, and record that list in this order
- [ ] `player_has_no_cook_stack` grows into `player_has_no_dev_machinery`: the symbol list covers every item above, and a mutation run (put one back) makes it fail
- [ ] `dist/` is checked after every `engine_build`: it fails on a source asset, `registry.db`, shader source, or a provider the project did not choose
- [ ] audio and scripts ship by reference (what scenes, materials and kits name), not by extension; anything referenced but missing is a warning, `--strict` makes it fatal
- [ ] Lua ships precompiled, or the order records why not
- [ ] the size report names each category (code, scenes, meshes, textures, audio, scripts) so growth is visible

## Contract
Nothing: a development build is unchanged. A shipped game loses only what it never used.

## Steps
1. Inventory: build the shipping player, list its symbols and open sockets, and diff against a dev build.
2. `ENGINE_SHIPPING` in CMake, with each item compiled out behind it.
3. The symbol check, then the folder check, both in CI on the Linux leg.
4. Reference-based packaging for audio and scripts.

## Not in scope
- The kit loader (`dlopen`) and the player's exported symbols (`ENABLE_EXPORTS`). Both exist because kits are loaded as shared libraries today. Removing them needs kits linked statically, which is WO-025, and the new API's `engLoadModule` (`docs/plans/engine-api.md`).
- Packaging for platforms other than desktop (today `run.sh` only).
- Crash reporting, which stays in a shipped game (`future-plans/crash-reporting.md`).
