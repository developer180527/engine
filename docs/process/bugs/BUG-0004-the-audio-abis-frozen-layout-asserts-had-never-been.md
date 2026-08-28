## BUG-0004 — the audio ABI's frozen-layout asserts had never been compiled
- found:     2026-08-22
- status:    fixed
- class:     coverage
- where:     include/engine/engine_audio_provider.h
- symptom:   none observable. Every static assert in the header was dead.
- cause:     the header carried ENGINE_AUDIO_FROZEN assertions and a comment claiming the C and C++ builds checked them, but NO translation unit in the tree included it. The Rust suite pinned only its own transcription, so the C structs could have been reshaped freely with every test still green.
- pinned-by: tests/audio_abi_check.c
- lane:      unit
- proof:     swapped two uint64 fields in EngineAudioStats — the build SUCCEEDED with every static assert blind to it, and only the new offset check caught it.
