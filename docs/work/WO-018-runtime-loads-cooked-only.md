---
status: plan
id: WO-018
title: Runtime loads cooked assets only — a missing one is a cook job, not an inline parse
program: assets
priority: P2
size: L
state: done
done: 2026-10-01
evidence: asset_cook_request_test (null -> Failed "not cooked"; cooker -> Pending, one cook request, Ready; cook failure -> Failed with the reason; a mesh waits for its texture; scene placeholder saved as the authored reference, then swapped; 3 mutations each red); player_has_no_cook_stack reads that test binary (no Assimp, cgltf or stb_image); audit IMP-01 with no baseline; 126 tests (all but the 10 fuzz explore campaigns)
depends: [WO-016, WO-017]
contracts: [asset-service]
new-contracts: [cook-request]
touches:
  - src/runtime/services/async_loader/parse.cpp
  - src/runtime/services/async_loader/loader.cpp
  - src/runtime/services/async_loader.h
  - src/runtime/runtime_boot.cpp
  - src/assets/cook_requests.h
  - src/assets/cookers/cook_service.cpp
  - src/scene/entity_serializer.h
  - src/components/mesh_placeholder.h
  - src/editor/panels/asset_browser/spawn.h
source: review 2026-09-29 R1, R2, R4, C5
---
## Why
When an asset has no cooked version, the runtime parses the source itself with a *different* parser.

So one asset can look different depending on whether it has been cooked yet. That looks like a rendering bug and is really a pipeline bug. It also means four parsers are kept consistent only by comments.

## Done when
- [x] removes `src/assets/importers/assimp_importer.cpp` and `src/runtime/services/async_loader/parse.cpp` from audit IMP-01's baseline (`scripts/audit_baseline.json`); the rule then holds for it without debt
- [x] the source-parsing path in `async_loader/parse.cpp` is deleted, including the disk search for textures by filename
- [x] `runtime_boot.cpp` registers no source importers in any runtime
- [x] a missing cooked asset in the editor becomes a cook request plus a placeholder, and the asset swaps in when the cook finishes
- [x] a test: an uncooked asset shows the placeholder, becomes ready after the cook, and there is no Assimp/cgltf call in the runtime process
- [x] the asset looks the same whether or not it was cooked before the editor opened, because there is only one path now

## Contract
This is the flagship **real / null / job** contract. `asset-service::request(id)` returns a handle that is **Pending**, **Ready** or **Failed**, and every caller handles all three.

Nothing: **job**. Pending shows the placeholder asset of that type, never a null pointer.
- **real** (editor): Pending → cook request to CookService → Ready.
- **null** (player, server): no cooker in this build, so a missing cooked asset is **Failed** immediately with "not cooked: <path>", logged once, and the placeholder stays.
- **fake** (tests): Ready or Failed after N ticks, scripted.

Ownership: the handle is a value. The asset it resolves to belongs to the service; callers never free it.

## Log
- 2026-10-01:
  - **Deleted.**
    - The AsyncLoader's Assimp parse, and its texture search (the source
      folder, `textures/`, names like `_Albedo`).
    - `GltfImporter`, `AssimpImporter`, `MeshImporter` and
      `ImporterRegistry` (and `importers` from both contexts).
    - The glTF branch in the scene serializer, and `gltf_losses.h`.
    - The `engine_source_import` library and `sourceimport::install`
      (WO-017's opt-in, now with nothing to opt in to).
    - The legacy `buildOzzClip(aiAnimation*)`, `import_test` and
      `stress_assets`, which tested only the deleted importers, and the
      `mesh-importer` contract.

    The cgltf and stb implementation TUs moved to `engine_cooking`, the only
    remaining user.
  - **The job.** `ICookRequests` (`src/assets/cook_requests.h`, contract
    `cook-request`) has three providers: real is `CookService::requestCook`
    (the asset is put in scope and cooked first), null is `NullCookRequests`
    or none (Failed at once, "not cooked"), and fake is in the test.
    - `AsyncLoader` is cooked-only and back in `engine_runtime`. A NotCooked
      result is parked and retried on the worker every 30 drains, since the
      worker is the only thread that reads the registry. It asks for each
      cook once.
    - It reports `state(path)` as Pending, Ready or Failed, and a Failed
      result carries `error`.
    - A cooked mesh whose materials name uncooked textures waits for those
      cooks too, so it never appears untextured and then changes.
    - A request that never lands fails after about 5 minutes.
  - **The placeholder.** A scene entity whose mesh is Pending or Failed
    shows the primitive cube. `MeshPlaceholder{shown}` records which handle
    is the stand-in, so `saveMesh` writes the authored reference while the
    renderer holds it, and a real mesh assigned by hand still wins. The
    editor's spawn creates the entity at once, with the placeholder and its
    authored reference, and swaps the mesh in when the cook lands.
  - **Hosts.** The editor hands its `CookService` to the loader (real).
    `engine_host` and `scene_resave` have no cooker (null): an uncooked
    asset is "not cooked", the reference is kept, and a save writes it back.
  - **Contract naming.** The order names `asset-service`, but
    "not cooked yet" can only happen for a SOURCE-path request, so the
    behaviour is `cook-request` on `AsyncLoader`. `asset-service` stays
    planned for `AssetService`'s own interface/null/fake split, and says so.
  - **Mutations** (each red on its own check, then restored):
    - saving the placeholder as a MeshRenderer writes `engine://primitive/cube`
      over the model;
    - not waiting for textures fails 4 checks;
    - re-requesting the cook every retry asks 4 times, not once.
  - **Test fixtures that encoded the old path.**
    - `async_loader_test` §2 used a POSIX file literally named
      `tri\angle.obj`, which the registry cannot find by any spelling. The
      fixture now cooks a normal file and requests it by a
      backslash-bearing raw path, as a Windows caller would.
    - `scene_mesh_reference_test` expected "no importer" wording.
  - **History.** BUG-0014's `where` (the deleted `gltf_importer.cpp`) is now
    `none`, with a note. WO-002 no longer names the retired contract.

