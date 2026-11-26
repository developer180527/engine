---
status: plan
id: WO-093
title: Conformance suites run against the providers a shipped player links
program: providers
priority: P2
size: M
state: todo
depends: [WO-088]
contracts: [audio-provider]
touches:
  - tests/audio_conformance/src/lib.rs
  - src/tools/engine_player.cpp
  - scripts/shipped_player_check.py
source: split out of WO-088 (2026-11-27); provider-selection.md §7–8 (DR-0037)
---
## Why
WO-088 packages real games and checks what their players link, but the conformance suites still run only against the development build. The one suite that exists, `tests/audio_conformance`, is a Rust crate that loads a provider as a shared library (`libengine_audio_miniaudio`). A shipped player links its providers statically, so there is nothing for the suite to load: the code players get is not the code the suite holds to the contract. The two builds compile the same source, but with different flags and a different link (DR-0032 strips the development machinery), and that gap is exactly where R7 of the plan says defects hide.

## Done when
- [ ] a shipped player can run a provider kind's conformance checks against its own linked provider, without dlopen (for example `engine_player --conformance <kind>`, compiled only into the players `shipped_player_check.py` builds, never into a game's)
- [ ] `shipped_player_check.py` runs them for every kind that has a suite (audio today) on both selections, and a failing check fails the CI step by name
- [ ] the Rust suite's checks and the in-player checks are one list, so a check added to one cannot be missing from the other; or the order records why they must differ
- [ ] a mutation run (a provider that breaks one rule of `audio-provider.md`) fails the in-player run

## Contract
Nothing: for games, the conformance entry point is absent from a game's player (DR-0032), present only in the packages CI builds to test.

## Not in scope
- Suites for physics and the renderer: each lands with its kind's contract (WO-074, WO-075), and is then added here.
