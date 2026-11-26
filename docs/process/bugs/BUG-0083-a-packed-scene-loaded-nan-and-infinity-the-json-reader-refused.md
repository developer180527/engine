## BUG-0083 — A packed scene loaded NaN and infinity the JSON reader refused
- found:     2026-11-27
- status:    fixed
- class:     parse
- where:     src/scene/component_schema.h
- symptom:   a corrupt or hand-edited packed cooked scene could put NaN or infinity into any float field (a transform, a light, a camera), and from there into physics and rendering. The same values in the JSON or CBOR form kept the field's default.
- cause:     the per-field rule (a value that is missing, wrong-typed or not finite keeps the default) was written into the JSON/CBOR field readers, and the packed readers for f32, f64 and the float vectors copied the bytes as they were. The reader-parity fuzzer compares the three readers only on what the packer writes, and the packer never writes a non-finite value, so it could not see the difference.
- pinned-by: tests/scene_schema_test.cpp
- lane:      unit
- proof:     section "packed floats that are not finite": a Transform record filled with NaN bytes loads as the default Transform, and one infinite element keeps its default while the rest load. Both checks fail on the previous reader (mutation-checked).
