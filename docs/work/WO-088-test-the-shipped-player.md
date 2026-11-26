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
- [ ] the conformance suites that exist (audio today, then each kind as its suite lands) run against the statically linked providers in that player, not only against the development build
- [ ] the symbol check confirms the packaged player contains the chosen providers and no other provider's symbols; a mutation run (link one extra provider) makes it fail
- [ ] a selection that cannot resolve on that leg (for example a provider not listed for it) is a test that expects the refusal, so the refusal path is exercised in CI too

## Contract
Nothing for users. CI gets slower by two packaging runs per leg; record the time it adds.

## Not in scope
- Consoles and phones as CI legs. They get the same check when they become targets.
- Performance of the shipped player: the home server's A/B (`provider-abi.md` §3, WO-083).
