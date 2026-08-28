## BUG-0008 — miniaudio's node-graph clock reported as a device clock
- found:     2026-08-22
- status:    fixed
- class:     logic
- where:     src/audio/miniaudio_provider.cpp
- symptom:   `samplesPlayed` frozen at 0. The conformance suite reported "0.0 ms of samples vs 125.2 ms of host".
- cause:     letting `ma_engine` create its own device meant reading `ma_engine_get_time_in_pcm_frames`, which is the NODE GRAPH's time and only advances while something is mixing. With nothing playing it does not move at all.
- pinned-by: tests/audio_conformance/src/lib.rs
- lane:      unit
- proof:     caught by BUG-0007's assertion one commit after that assertion was written. Fixed by having the provider own the ma_device and count frames in its own callback.
