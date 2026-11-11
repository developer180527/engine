---
status: plan
id: WO-076
title: A provider registry — providers chosen by name from one list, an unknown name refused
program: providers
priority: P1
rank: 2
size: M
state: todo
depends: []
contracts: [engine-plugin]
touches:
  - src/plugins/stock_plugins.h
  - src/capi/eng_capi.cpp
  - src/project/project_context.h
  - src/runtime/runtime_boot.cpp
source: review 2026-11-11 (swappable providers at development time); split out of WO-074; DR-0029, DR-0035
---
## Why
Providers are chosen by an if-chain in `stock_plugins.h`, and the same names are written a second time in `eng_capi.cpp`. A misspelled name in `project.json` silently runs Jolt or Lua instead (DR-0035 says refuse). The renderer is not chosen by name at all (`runtime_boot.cpp`). A new provider means editing engine code in two places. One registry fixes all four, and it needs nothing from the world API, so it does not have to wait for WO-073 as WO-074 does.

## Done when
- [ ] a registry in the runtime: a provider registers a kind (`physics`, `audio`, `scripting`, `renderer`), a name and a factory. Registration is an explicit call from one list per build, not a static initialiser (a static library drops objects nobody references, so self-registration silently loses providers)
- [ ] `stock_plugins.h`'s if-chain and `eng_capi.cpp`'s name lists are gone: both ask the registry, and the refusal message's list of known names comes from it
- [ ] an unknown provider name is refused at startup, by name, with the names this build has, for `project.json` and for `EngEngineDesc.providers`; tested for each kind
- [ ] direct injection: a host may hand the engine a provider object without a name (`engine-architecture.md` §2b), tested with a fake physics provider
- [ ] the renderer is in the registry as `forward-bgfx` and `null`, chosen by `project.json` (`"renderer"`), with today's rule kept: no display or a server build gets `null`, and says so
- [ ] `engine_player --list-providers` prints kinds and names, with `--json`

## Contract
Nothing: a project that names `jolt`, `lua`, `miniaudio` or `none` behaves as today. Only a misspelled name changes, from a silent fallback to a refusal that names it. Existing `project.json` files with no `"renderer"` get the default.

## Not in scope
- Loading providers from shared libraries, and swapping without a restart: WO-077.
- Moving physics off flecs (WO-074) and the renderer contract (WO-075). The registry holds today's provider classes as they are.
- Services of new kinds (`engFindService`, `engine-api.md`): the registry's kinds stay a fixed list until the C API's module part lands.
