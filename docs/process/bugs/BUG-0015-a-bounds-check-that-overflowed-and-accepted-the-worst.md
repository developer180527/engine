## BUG-0015 — a bounds check that overflowed and accepted the worst input
- found:     2026-08-24
- status:    fixed
- class:     memory
- where:     tests/cooked_format/src/shader.rs
- symptom:   at `offset = u32::MAX` a debug build panicked ("attempt to add with overflow") and a RELEASE build wrapped to a small number and returned Ok — the bounds check accepting the most out-of-bounds value representable, which is exactly the input it exists to reject.
- cause:     `params_fit_their_uniforms` computed `offset + components` in u32 on an offset read RAW FROM THE FILE. Its sibling `variant_ranges_in_bounds`, two functions above in the same file, had already been widened to u64 — nothing held the pair together, so they drifted apart in a single sitting.
- pinned-by: tests/cooked_format/tests/cshader_ddc.rs
- lane:      unit
- proof:     mutation in BOTH profiles — debug reports "attempt to add with overflow" at shader.rs:177, release fails the assertion with "the addition wrapped and the check accepted the worst possible input". The test pins BOTH functions plus the in-bounds boundaries either side, because the defect is the drift between siblings rather than the arithmetic in one of them.
