## BUG-0025 — a cross-thread field hidden under a comment saying it was not one
- found:     2026-08-25
- status:    fixed
- class:     threading
- where:     src/audio/miniaudio_provider.cpp
- symptom:   none reported by TSan; found by reading after it flagged BUG-0024.
- cause:     expectedPeriodNs sat under the comment "Callback-local, touched only by the audio thread" and is computed in maCreate. Grouping a cross-thread field beneath a comment ASSERTING it cannot be one is how it stayed invisible — a reader checking for races skips the block the comment excludes.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      tsan
- proof:     now an atomic, loaded relaxed in the callback. Worth noting TSan found one race and reading found the other: the tool bounds what it can observe in a single run, so a report is a starting point rather than a complete list.
