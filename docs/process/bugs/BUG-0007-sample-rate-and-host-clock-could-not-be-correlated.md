## BUG-0007 — sample rate and host clock could not be correlated
- found:     2026-08-22
- status:    fixed
- class:     abi
- where:     include/engine/engine_audio_provider.h
- symptom:   `startSampleTime` was decorative — the engine could not compute a future sample instant.
- cause:     `EngineAudioStats` reported `samplesPlayed` with no host timestamp. A count is not a mapping: the game thread learned how many samples had played but not WHEN that was true, and an arbitrary amount of time passed between publish and read.
- pinned-by: tests/audio_conformance/src/lib.rs
- lane:      unit
- proof:     the suite checks the two clocks agree on elapsed time, which is what proves they were sampled together rather than merely both present.
