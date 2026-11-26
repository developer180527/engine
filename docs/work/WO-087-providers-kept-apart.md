---
status: plan
id: WO-087
title: Providers kept apart — engine services between them, one create context, no globals
program: providers
priority: P2
size: S
state: todo
depends: []
touches:
  - src/core/debug_draw.h
  - src/plugins/jolt_plugin.h
  - src/plugins/audio_plugin.h
  - scripts/engine_audit.py
source: provider-selection.md §6 (DR-0037); provider-abi.md rule 9; research 2026-11-26 (CryEngine gEnv; Bevy's implicit plugin dependencies; O3DE's DebugDisplayRequestBus)
---
## Why
Swapping one provider must never drag another with it. Today nothing stops a provider from including another provider's header, and the engine's debug-draw collector, the service physics would use to show its shapes, packs colours in bgfx's layout and includes `bx`. CryEngine's `gEnv` shows where a shared global environment leads: every module reaches every other. Bevy's own developers call its plugin dependencies "implicit even when reading the code". Engines that stay decoupled route cross-provider traffic through a service the engine owns, as O3DE does for debug drawing.

## Done when
- [ ] audit rule `PROV-01`: a file under a provider's directory includes only core, service and its own headers, never another provider's; a mutation run (add one such include) makes it fail
- [ ] `src/core/debug_draw.h` stops including `bx` and packs colours in an engine-defined layout; the renderer converts. Physics can draw debug shapes without knowing which renderer runs
- [ ] each provider's create call takes one context struct (host services, log, allocator tags, jobs, and the surface for a renderer), and providers reach nothing global. Listed: what each provider reaches today, before and after
- [ ] a test runs physics debug drawing with the `none` renderer and with `forward-bgfx`, unchanged

## Contract
Nothing visible: providers behave as today. Only code that reached across providers has to change, and the list of such code is recorded in this order first.

## Not in scope
- The CMake half (a provider target linking another provider's target): WO-085.
