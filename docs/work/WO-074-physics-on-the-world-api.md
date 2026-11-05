---
status: plan
id: WO-074
title: Physics as the first provider on the world API — and providers chosen from a registry that refuses by name
program: providers
priority: P1
rank: 5
size: M
state: todo
depends: [WO-073]
contracts: [engine-plugin]
touches:
  - src/plugins/jolt_plugin.h
  - src/plugins/stock_plugins.h
source: docs/plans/engine-architecture.md §8 steps 5 and 7; roadmap §2.8; DR-0029, DR-0035
---
## Why
Physics is the one provider that reaches into flecs directly (`jolt_plugin.h` takes `flecs::world`), and providers are chosen by a hard-coded switch that silently falls back to Jolt when `project.json` names a provider it does not know (`stock_plugins.h`). Physics is the smallest real provider, so it proves the world API for providers before the renderer contract does.

## Done when
- [ ] the Jolt plugin reads and writes bodies through the world API and the change stream, with no flecs include; `physics_capacity_test`, the determinism gate and the collision-event tests pass unchanged
- [ ] marked writes in use: only awake bodies' transforms are reported (WO-072's test, on the real plugin)
- [ ] a provider registry: providers register by name and service; `project.json` chooses among them; an unknown name is refused at startup with the list this build has (DR-0035), tested
- [ ] the null physics provider passes the same conformance tests
- [ ] the per-step cost on the 2 000-box pile measured before and after, and recorded

## Contract
Nothing: projects that name `jolt` or `none` behave as today; only a misspelled name changes, from a silent fallback to a refusal.
