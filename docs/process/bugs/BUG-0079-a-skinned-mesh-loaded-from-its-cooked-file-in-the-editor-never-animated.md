## BUG-0079 — A skinned mesh loaded from its cooked file in the editor never animated
- found:     2026-11-26
- status:    fixed
- class:     logic
- where:     src/scene/entity_serializer.h
- symptom:   a scene whose skinned mesh had a cooked path loaded it in the editor with an invalid skeleton: SkinnedMesh and Animator were set from the file, but no skeleton or clip handle was ever wired, so the character stood in bind pose. Only meshes loaded through the import callback (no cooked path yet) animated.
- cause:     loadMeshRef's cooked fast path asked SceneAssets::loadCookedMesh for the LOD chain only; the hook had no way to return the skin. The import callback and the shipped player's own loader each wired the skin themselves, in different code.
- pinned-by: tests/scene_schema_test.cpp
- lane:      unit
- proof:     the hook returns the skin, the mesh's entity is wired once its authored Animator has been read (PendingSkin, wireSkin), and the import callback and the player use the same wireSkin. Dropping the PendingSkin fails 4 checks.
