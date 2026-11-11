---
status: plan
id: WO-077
title: Swap a provider at development time — loaded by name, changed when the simulation stops
program: providers
priority: P2
size: L
state: todo
depends: [WO-076]
contracts: [engine-plugin, audio-provider]
touches:
  - src/plugins/audio_plugin.h
  - src/audio/audio_host_services.h
  - src/tools/engine_host.cpp
  - src/runtime/module_loader.h
  - src/CMakeLists.txt
source: review 2026-11-11 (swappable providers at development time); engine-architecture.md §2b; DR-0029, DR-0032
---
## Why
A developer should be able to try another physics, audio or renderer provider without editing engine code or rebuilding the engine: change a name, press play. In a shipped game the opposite holds: providers are linked in and nothing can be swapped (DR-0032). The audio provider already builds as a separate shared library that only the conformance suite loads, and providers already build their state from the world at `onSimulationStart` and drop it at `onSimulationStop`. So the pieces exist; they are not connected.

## The rules
- **Same provider source, two link modes.** A development build may also build each provider as a shared library; a shipped build links them statically. No frozen binary contract is needed (DR-0006): the engine and its providers are built together, by one compiler.
- **Swap when the simulation is stopped.** Stop, detach the old provider, attach the new one, play. Nothing a provider owns carries over, because the world and cooked files hold only intent (DR-0031).
- **One allocator, one job pool, one log.** A provider library resolves engine symbols from the host program, as kits do. It never carries its own copy of the engine (`libeng` aborted for exactly this reason: memory crossing two allocators, `eng_shared_test`).
- **Not mid-play.** Physics cannot hand contacts and velocities to another library. A live swap is only worth having for the renderer, and is a later order.

## Done when
- [ ] audio: in a development build the registry (WO-076) can load an audio provider from a shared library by name (`"audio": "miniaudio"` finds `providers/libengine_audio_miniaudio`), through the module loader's checks; the conformance suite and the engine load the same file
- [ ] a provider is swapped while the simulation is stopped, in the editor and in `engine_host`: the old one detaches, the new one attaches, and the next play uses it. Tested headless: play with `jolt`, stop, swap to `none`, play (nothing falls), swap back, play (same world hash as a fresh `jolt` run)
- [ ] a swap asked for while the simulation runs is refused by name, not queued silently
- [ ] `engine_host --provider physics=none` and the same in `engine_player` (development builds only) override `project.json` for one run
- [ ] a provider that needs a cook profile the project has not cooked asks for it as a cook request (WO-018), and the swap completes when the cook does
- [ ] under the shipping build setting (WO-066) the loader and the swap are absent from the binary, checked by the symbol check
- [ ] the editor shows the provider per kind and the capabilities it reports, and greys out what the chosen one lacks

## Contract
Nothing: without a provider library on disk, the registry's linked-in providers are used, as today. A swap that fails leaves the old provider attached and reports why.

## Not in scope
- Physics and the renderer as separately loadable libraries: each needs its contract first (WO-074, WO-075). Until then they swap among the providers linked into the build.
- Live renderer swapping during play, for A/B comparison.
- Third-party providers shipped as binaries (DR-0006, `engine-architecture.md` §9.3).
