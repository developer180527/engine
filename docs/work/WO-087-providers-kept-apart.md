---
status: plan
id: WO-087
title: Providers kept apart — engine services between them, one create context, no globals
program: providers
priority: P2
size: S
state: done
done: 2026-11-27
evidence: audit PROV-01 and PROV-02 (mutation-checked: a planted include of another provider's header, and of runtime/runtime.h, each named), tests/provider_debug_draw_test.cpp
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
- [x] audit rule `PROV-01`: a file under a provider's directory includes only core, service and its own headers, never another provider's; a mutation run (add one such include) makes it fail
- [x] `src/core/debug_draw.h` stops including `bx` and packs colours in an engine-defined layout; the renderer converts. Physics can draw debug shapes without knowing which renderer runs
- [x] each provider's create call takes one context struct (host services, log, allocator tags, jobs, and the surface for a renderer), and providers reach nothing global. Listed: what each provider reaches today, before and after
- [x] a test runs physics debug drawing with the `none` renderer and with `forward-bgfx`, unchanged

## Contract
Nothing visible: providers behave as today. Only code that reached across providers has to change, and the list of such code is recorded in this order first.

## Not in scope
- The CMake half (a provider target linking another provider's target): WO-085.

## What each provider reaches (recorded 2026-11-27)

| Provider | Before | After |
|---|---|---|
| physics `jolt` | core (mem, jobs, log), components, the script service interfaces. No context. No debug drawing. | the same, plus `ProviderContext::debugDraw`, the engine's collector, for `setDrawBodies` |
| physics `none`, scripting `none`, audio `none` | the log | the same |
| scripting `lua` | `RuntimeContext` at attach (project roots and ScriptHost), and `project_context.h` for the paths | `ProviderContext` at creation: the roots and ScriptHost. `project_context.h` is gone |
| audio `miniaudio` | `RuntimeContext` at attach (the assets root), core, its host services | `ProviderContext` at creation: the assets root |
| renderer `forward-bgfx` | drained the collector by `memcpy`, relying on its vertex layout matching bgfx's | converts field by field (`render/pipeline/debug_lines.h`) |
| the collector (`core/debug_draw.h`) | `bx::Vec3` and bx's maths, colours packed in bgfx's ABGR | its own `dbg::Vec3` and `dbg::Color` (RGBA bytes); no bx. RHI-01's accepted debt fell from 70 to 68 |

No provider included another provider's header before this order, and none
does now; PROV-01 keeps it that way.

## Notes (done 2026-11-27)
- **The SDK's direct form stays.** `include/engine/plugins.h` (frozen) documents `std::make_shared<LuaScriptPlugin>()` and `AudioPlugin()`. Made with no context, those two still take the same few things from the `RuntimeContext` they are attached with. Only the registry's way, `ProviderContext`, is the new contract. Retiring the old form is a version bump of the SDK, not this order.
- **What "nothing global" means here.** Providers still call core's services directly (`mem::`, `jobs::`, the log). Those are the engine's own services, and every provider is meant to use them (provider-abi.md rule 6). What is now refused is the runtime's internals (PROV-02) and other providers (PROV-01).
- **The bgfx renderer needs a window, which a test cannot have.** So the test checks the renderer's half that reads the collector (`toLineVertices`), and physics drawing end to end with the `none` renderer.
- **The collector is cleared in `frameBegin`**, so a loop that calls only `tick()` keeps adding lines. The test drives whole frames, as the host loop does.
