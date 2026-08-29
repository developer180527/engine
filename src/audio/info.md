---
status: as-built
tier: working
verified: 2026-08-29
covers:
  - src/audio/
tests:
  - tests/audio_provider_asan_test.cpp
  - tests/audio_conformance/tests/conformance.rs
---
# src/audio — the engine's side of the audio provider boundary

Two files, and they are two different jobs:

| file | job |
|---|---|
| `audio_host_services.h` | the ENGINE's half — hands a provider the allocators, the job pool and the clock through `EngineAudioHostServices` |
| `miniaudio_provider.cpp` | the first PROVIDER — miniaudio behind `EngineAudioProviderV1`, built both statically and as a standalone `.so` |

The design rationale lives in the file headers and in
[`docs/guides/audio-provider.md`](../../docs/guides/audio-provider.md); this
document is the subsystem's contract with `engine_doctor` and the place its
defect history is summarised.

## Why this document exists

**There was no `info.md` here until 2026-08-29.** The subsystem did not appear on
`ENGINE_STATUS.md` at all, so it had no tier, no `verified:` date, and no
staleness check — while carrying the densest defect history in the engine.
`docs/plans/subsystem-audit.md` §3.1 records it as Rank 0 for exactly that reason:
you cannot rank what you cannot see, and this was invisible.

## The defect history, and how to read it

**Five of the engine's forty-four ledger entries name
`miniaudio_provider.cpp`** — one source file:

| | |
|---|---|
| BUG-0008 | miniaudio's node-graph clock reported as a device clock |
| BUG-0022 | `maRealloc` read past the end of every block it grew |
| BUG-0024 | `engineInit` published without a happens-before edge |
| BUG-0025 | a cross-thread field hidden under a comment saying it was not one |
| BUG-0026 | an overrun counter that measured the runner instead of the mixer |

Two more (BUG-0019, BUG-0020) come from the conformance suite itself.

**Density here measures attention as much as defects**, and saying so is not a
hedge — it is the finding. This is one of very few subsystems with a Rust
conformance suite *and* a dedicated ASan lane pointed at it, which is precisely
why its defects are written down. Four of the five are threading or memory bugs
in code that runs on a real-time callback thread; that is the risk profile of the
subsystem, not evidence that it is the worst code in the tree. The subsystems with
*zero* entries are the ones nobody has aimed a lane at.

## Tier: `working`, and what would make it `hardened`

The evidence supports `working`: two real test lanes exist, both pass, and both
are registered in ctest.

| lane | what it holds |
|---|---|
| `audio_abi_conformance` (Rust) | dlopens the provider `.so` and holds it to the ABI: struct hash matches the C header, the reference and native providers both conform, and the native one **actually produces audio** rather than merely returning success |
| `audio_provider_asan` | the provider's allocator under ASan — the lane BUG-0022 came out of |
| `audio_abi_check` | the frozen-layout assertions compile and hold |

`hardened` additionally requires an endurance lane, and specifically a **fuzz**
lane for a subsystem that parses external input. This one does: `createSound`
takes encoded bytes and `ma_decoder` parses WAV, MP3, FLAC and Vorbis from them.
So the gap is precise and worth stating rather than leaving as "needs more tests":

> **A fuzz target over `createSound`'s encoded-bytes path.** Malformed audio is
> untrusted input reaching a third-party decoder on a path a game runs at load
> time, and nothing currently feeds it garbage. This is the single highest-value
> test this subsystem does not have.

The conformance suite substitutes a counting allocator, so a fuzz lane could reuse
that harness rather than building a new one — the decode path already runs behind
an interface that can be driven from Rust.

## The property that must not rot

`miniaudio_provider.cpp` includes **exactly two things**: miniaudio and the ABI
header. No engine symbol, no engine header. That is what makes it a provider a
third party could have written rather than more engine wearing a provider's name,
and it is enforced structurally — the `engine_audio_miniaudio_module` target links
against nothing, so a stray engine include fails the link rather than passing
review.

Both builds come from one source, so the conformance suite tests the code the game
actually runs. Deleting the module target would leave the static build passing
while the standalone property silently rotted; it exists for that reason and not
because anything dlopens it in a shipping game.

## Future Work
- The fuzz lane above, which is what `hardened` is blocked on.
- A second provider (FMOD, Wwise, or a null/deterministic one for tests) is the
  only real proof the ABI generalises. `docs/guides/audio-provider.md` is written
  for that reader; nobody has been that reader yet.
- Nothing here is `production`: that additionally needs every CI platform and a
  perf claim backed by a test, and the audio thread's deadline behaviour is not
  measured on any platform but macOS today.
