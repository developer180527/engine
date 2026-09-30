## BUG-0066 — A cooked glTF spawned in the editor without its skeleton, so it could not animate
- found:     2026-09-30
- status:    fixed
- class:     logic
- where:     src/editor/panels/asset_browser/spawn.h, src/runtime/services/async_loader.h
- symptom:   dragging a skinned, animated .glb into the scene spawned a static mesh: no SkinnedMesh, no Animator, nothing to play. The asset had cooked correctly, with its bones and clips. Other formats did not show it.
- cause:     `spawnFile` sent every .gltf/.glb through the runtime glTF importer (`ctx.importers.loadCached`), which reads static geometry only and says so in a warning (`gltf_losses.h`). The async loader, which other formats take, prefers the cooked file and decodes its skeleton and clips, but glTF never went there.
- pinned-by: tests/async_loader_test.cpp
- lane:      unit
- proof:     `async_loader_test` §3 cooks a skinned glTF through the real pipeline and requires `hasCooked()` false before the cook and true after, and the cooked load to return the mesh, its skeleton and its clip. With `hasCooked()` stubbed to false it is red. Fixed in `103c66f` (WO-036): `AsyncLoader::hasCooked`, and a glTF with a Ready cooked version spawns through the cooked path. What the test cannot reach is `spawnFile`'s own one-line branch, which needs the editor's ImGui context; WO-018 deletes the uncooked branch it chooses between.
