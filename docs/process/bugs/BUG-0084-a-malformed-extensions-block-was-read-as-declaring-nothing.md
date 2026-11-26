## BUG-0084 — A malformed "extensions" block was read as declaring nothing
- found:     2026-11-27
- status:    fixed
- class:     logic
- where:     src/project/project_context.h
- symptom:   a project whose `"extensions"` block had a misspelled key (`"requried"`), a list written as a string, or a block that was not an object started Play with no check at all, so a required extension it lacked was never reported. A test pinned this as intended ("a malformed block declares nothing").
- cause:     the loader skipped anything that was not a list of names, silently, against DR-0035 (refuse by name, never fall back silently).
- pinned-by: tests/scene_extensions_test.cpp
- lane:      unit
- proof:     the loader records each problem (`Extensions::malformed`), and startSimulation refuses with it by name, as it does a missing required extension. The project still opens in the editor. Cases: a list that is not names, a misspelled key, a block that is not an object.
