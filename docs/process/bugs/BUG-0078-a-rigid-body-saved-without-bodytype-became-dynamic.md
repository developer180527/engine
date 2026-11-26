## BUG-0078 — A rigid body saved without bodyType became dynamic
- found:     2026-11-26
- status:    fixed
- class:     logic
- where:     src/scene/entity_serializer.h
- symptom:   a `"rigidBody"` with no `"bodyType"` loaded as dynamic in the editor, so a body meant to sit still fell. The format has always read it as kinematic (the old serde table, the cooker, docs/reference/scene-format.md). Scenes the editor writes always carry bodyType, so only hand-written or hand-edited ones were affected.
- cause:     introduced by WO-071 (0d02b84) in registerBuiltinSchemas: the generic serializer starts from the struct's own defaults, and RigidBody's is dynamic; the hand-written loader it replaced read `j.value("bodyType", 1)`. WO-071 checked every other component's struct defaults against its old loader and missed this one.
- pinned-by: tests/scene_schema_test.cpp, tests/sdk_only_game_test.cpp
- lane:      unit
- proof:     a schema can now state its own defaults (`SchemaBuilder::defaults`), and the rigid body's says kinematic; the schema's reported defaults agree. Removing that line fails 2 checks.
