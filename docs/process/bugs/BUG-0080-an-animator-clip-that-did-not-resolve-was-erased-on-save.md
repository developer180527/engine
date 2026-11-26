## BUG-0080 — An animator clip that did not resolve was erased on save
- found:     2026-11-26
- status:    fixed
- class:     logic
- where:     src/scene/entity_serializer.h
- symptom:   an Animator whose standalone clip could not be found on this machine (moved, not checked out, a teammate's path) loaded with an empty clipPath, and the next save wrote the animator without its clip reference. The same read in the shipped player, which has no source assets, would have found no clip at all.
- cause:     loadAnimator, in disk mode, replaced the authored reference with `assetref::resolve(...)`, which is empty when nothing resolves here.
- pinned-by: tests/scene_schema_test.cpp
- lane:      unit
- proof:     an unresolved reference keeps its authored path (DR-0034); the clip library maps it to a packaged clip where one exists, and a save writes it back.
