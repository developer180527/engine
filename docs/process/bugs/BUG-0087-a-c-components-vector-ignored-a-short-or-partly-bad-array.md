## BUG-0087 — A C component's vector ignored a short or partly bad array
- found:     2026-11-28
- status:    fixed
- class:     logic
- where:     src/scene/dynamic_schema.h
- symptom:   a component registered through the C API read a vector field (VEC2/3/4, QUAT, MAT4) only when the saved array had exactly the right length and every element was a finite number; otherwise the whole field kept its default. An engine component reads the same array element by element: `[1, 2]` into a VEC4 is (1, 2, default, default), and a bad element keeps only its own default. dynamic_schema.h says its rules are SchemaBuilder's, and docs/reference/scene-format.md promises the element-wise rule for every component.
- cause:     `dyn::loadValue` required `v.size() == n` and returned on the first non-number.
- pinned-by: tests/eng_world_scene_test.cpp
- lane:      unit
- proof:     both cases returned (0, 0, 0, 0) before the fix and the documented values after.
