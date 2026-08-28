## BUG-0026 — an overrun counter that measured the runner instead of the mixer
- found:     2026-08-25
- status:    fixed
- class:     logic
- where:     src/audio/miniaudio_provider.cpp
- symptom:   "NO callback overruns under a 2000-command burst" failed on the Linux CI legs, 9 overruns counted — an assertion about our mixer failing on a fact about the machine.
- cause:     a Linux runner has no sound card: ALSA prints "cannot find card '0'" and then returns a working 48 kHz device anyway, whose callback cadence is whatever the scheduler felt like. An overrun means "we missed a HARDWARE deadline"; with no hardware there is nothing to miss.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      unit
- proof:     ENGINE_AUDIO_NO_HARDWARE=1 covers both shapes of "no dependable audio device", permits the null-backend fallback and stops the timing assertions. ctest sets it; a shipped game never does, so a real device failure stays visible as E_NO_DEVICE. The FIRST version of the flag meant only "we fell back to the null backend" and could never trigger, because ALSA had already succeeded.
