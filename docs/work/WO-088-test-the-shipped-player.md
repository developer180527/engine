---
status: plan
id: WO-088
title: Test what ships — the static shipped player built and run in CI, per platform and selection
program: providers
priority: P2
size: M
state: todo
depends: [WO-086]
touches:
  - .github/workflows/ci.yml
  - tests/CMakeLists.txt
  - src/tools/engine_build.cpp
source: provider-selection.md §8 (DR-0037); research 2026-11-26 (O3DE's monolithic shipping build turns its tests off)
---
## Why
A shipped game links its providers statically from a generated list. A development build loads them differently. If the tests only ever run the development build, the build that players get is the one nobody tests. O3DE is the example: its monolithic game build switches tests off. The shipped player has to be built and run in CI, with the selections games actually use.

## Done when
- [ ] on every CI desktop leg, `engine_build --platform <that leg>` packages a sample for two selections, the default and `server`, and the job runs the packaged player headless on a smoke scene
- [x] the conformance suites that exist run against the statically linked providers in that player: **moved to WO-093**. The one suite (audio) is a Rust crate that loads a provider as a shared library, and a static player has none to load
- [x] the symbol check confirms the packaged player contains the chosen providers and no other provider's symbols; a mutation run (link one extra provider) makes it fail
- [x] a selection that cannot resolve on that leg (for example a provider not listed for it) is a test that expects the refusal, so the refusal path is exercised in CI too

## Contract
Nothing for users. CI gets slower by two packaging runs per leg; record the time it adds.

## Not in scope
- Consoles and phones as CI legs. They get the same check when they become targets.
- Performance of the shipped player: the home server's A/B (`provider-abi.md` §3, WO-083).

## Notes (2026-11-27, waiting on CI; left `todo`, not `active`: two orders are active already)
- **Built:** `scripts/shipped_player_check.py`, and CI's "Shipped player" step on every leg when the full matrix runs (nightly, on demand, portability-sensitive pushes). It scaffolds `basic3d` twice: the engine's defaults, and **headless** (renderer `none`, audio `none`). "Headless" stands where this order said `server`, because `engine_build` refuses `--flavour server` until a server player exists. Each is packaged with `engine_build --strict`. The step checks `providers.lock.json`, then the player's symbols against it (chosen providers present, others and the resolver absent). It runs the headless package for 120 frames with `engine_player --headless --frames N`, which is new. The default package must refuse to start with no display, by name. CI has no display, so the default package is built and checked but not run.
- **Mutation:** ctest `shipped_player_check_refuses_extra_providers` runs the symbol check on the development player, which has every provider, against a lock that chose fewer. It must fail, and does, naming each extra provider and the resolver.
- **Refusal in CI:** `engine_build_refuses_web_by_name` (WO-086) already runs on every leg's unit lane.
- **Found on the first local run:** `NullRenderer` is in every player, because the runtime installs it at construction so that calls before `init()` are safe. So renderer `none` has no symbol marker. Its presence is not a sign of the choice.
- **engine_build on Windows:** never run before. It now copies `engine_player.exe` and configures a new shipping tree with the dev tree's generator (Ninja), not CMake's default (Visual Studio).
- **Local (macOS):** 121 s cold, 15 s warm per package. The CI time is recorded in the step summary once it runs.
- **Open:** the first item, until CI has run the step green on all six legs.
