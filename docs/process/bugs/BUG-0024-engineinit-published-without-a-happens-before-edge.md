## BUG-0024 — engineInit published without a happens-before edge
- found:     2026-08-25
- status:    fixed
- class:     threading
- where:     src/audio/miniaudio_provider.cpp
- symptom:   TSan reported a race between maCreate and the CoreAudio callback thread.
- cause:     engineInit was a plain bool, written by the creating thread and read by the callback. The ORDERING in the source was already correct — set before ma_device_start — but source order is not a happens-before edge, and ma_device_init has already created the callback thread by that point, so nothing made the write visible.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      tsan
- proof:     a release store paired with a SINGLE acquire load at the top of the callback, which orders everything else maCreate published.
