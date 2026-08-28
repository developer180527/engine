## BUG-0020 — a process-global counter driven by two parallel tests
- found:     2026-08-24
- status:    fixed
- class:     threading
- where:     tests/audio_conformance/src/reference.rs
- symptom:   `reference_stride_is_walked_by_stride` could read the other reference test's value and report a stride bug that never happened.
- cause:     LAST_WELL_FORMED is process-global and both reference tests drive it; cargo runs tests on parallel threads by default.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      unit
- proof:     serialised behind REFERENCE_LOCK, so the counter is read by the test that wrote it.
