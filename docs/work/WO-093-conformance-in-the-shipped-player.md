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
  - tests/audio_conformance/tests/conformance.rs
  - tests/audio_conformance/ffi/src/lib.rs
  - src/tools/engine_player.cpp
  - src/tools/player_conformance.cpp
  - src/tools/engine_build.cpp
  - src/CMakeLists.txt
  - scripts/shipped_player_check.py
source: split out of WO-088 (2026-11-27); provider-selection.md §7–8 (DR-0037)
---
## Why
WO-088 packages real games and checks what their players link, but the conformance suites still run only against the development build. The one suite that exists, `tests/audio_conformance`, is a Rust crate that loads a provider as a shared library (`libengine_audio_miniaudio`). A shipped player links its providers statically, so there is nothing for the suite to load: the code players get is not the code the suite holds to the contract. The two builds compile the same source, but with different flags and a different link (DR-0032 strips the development machinery), and that gap is exactly where R7 of the plan says defects hide.

## Done when
- [x] a shipped player can run a provider kind's conformance checks against its own linked provider, without dlopen (for example `engine_player --conformance <kind>`, compiled only into the players `shipped_player_check.py` builds, never into a game's)
- [x] `shipped_player_check.py` runs them for every kind that has a suite (audio today) on both selections, and a failing check fails the CI step by name
- [x] the Rust suite's checks and the in-player checks are one list, so a check added to one cannot be missing from the other; or the order records why they must differ
- [x] a mutation run (a provider that breaks one rule of `audio-provider.md`) fails the in-player run

## Contract
Nothing: for games, the conformance entry point is absent from a game's player (DR-0032), present only in the packages CI builds to test.

## Not in scope
- Suites for physics and the renderer: each lands with its kind's contract (WO-074, WO-075), and is then added here.

## Notes (2026-11-27; left `todo`, not `active`: two orders are active already. Close once CI is green on every leg)
- **How it runs without dlopen:** the Rust suite also builds as a static library, `tests/audio_conformance/ffi`, with one C entry point, `engine_audio_conformance_run(table)`. A player built with `ENGINE_PLAYER_CONFORMANCE` links it. `engine_player --conformance audio` hands it the table the player itself linked (`engineAudioProviderV1()`), so the code under test is the shipped player's own, built with the shipping flags.
- **Only in test packages:** `engine_build --conformance` turns the option on. engine_build states it ON or OFF on every configure, because the shipping tree is reused and a cached ON must never reach a game. `providers.lock.json` records `"conformance"`. A player without it refuses `--conformance` by name.
- **One list:** `engine_audio_conformance::SUITE` (contract, playback) is the list. `cargo test`'s native test walks it against a loaded module, and the static library walks it against the linked provider. The two native tests became one, `native_provider_passes_the_suite`. There is nothing for the in-player side to forget.
- **The two selections, not both with the suite:** the order asked for the suite "on both selections". The headless selection chooses audio `none`, which has no provider table to hold to the contract. So the default package is built with `--conformance` and runs the suite and the mutation, and the headless package is built as every game is. Its symbols must not contain the suite, and its player must refuse the flag by name. That makes the step check both halves of the contract above: the suite is in a test package, and absent from a game's.
- **The mutation:** `--mutate` wraps the linked table's `play` so it returns a voice id where the provider said NO_VOICE, the rule audio-provider.md states for an invalid sound. The in-player run fails on exactly "playing an unknown sound yields NO_VOICE", with exit 1, and the step requires that.
- **Local (macOS):** `shipped_player_check.py` passes: the default package runs 49 checks against miniaudio and catches the mutation; the headless package lacks the suite and refuses the flag. 21 s and 15 s per package on a warm tree. The Rust library builds in about a second.
- **Watch on CI:** this is the first Rust code linked into a C++ executable on the Windows legs outside a test. Its system libraries are the list `modules/net` uses. The step runs in bash on Windows, but a Rust static library is never linked by rustc, so the wrong `link.exe` on that PATH (see ci.yml) does not apply.
- **CI's first run (fdfb98e):** all three Linux legs failed the in-player audio suite, and the mutation was still caught. The suite ran without `ENGINE_AUDIO_NO_HARDWARE=1`, which ctest has always given the cargo suite. No runner has a sound card, and on Linux ALSA hands back a dummy device whose timing is the scheduler's. The script now runs the suite with it, as ctest does; a game's player never sets it. The failing line itself was lost under ALSA's complaints about the missing card, so the script now prints only the suite's own lines.
