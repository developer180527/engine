---
status: plan
id: WO-018
title: Runtime loads cooked assets only — a missing one is a cook job, not an inline parse
program: assets
priority: P2
size: L
state: todo
depends: [WO-016, WO-017]
contracts: [asset-service]
touches:
  - src/runtime/services/async_loader/parse.cpp
  - src/runtime/runtime_boot.cpp
source: review 2026-09-29 R1, R2, R4, C5
---
## Why
When an asset has no cooked version, the runtime parses the source itself with a *different* parser.

So one asset can look different depending on whether it has been cooked yet. That looks like a rendering bug and is really a pipeline bug. It also means four parsers are kept consistent only by comments.

## Done when
- [ ] removes `src/assets/importers/assimp_importer.cpp` and `src/runtime/services/async_loader/parse.cpp` from audit IMP-01's baseline (`scripts/audit_baseline.json`); the rule then holds for it without debt
- [ ] the source-parsing path in `async_loader/parse.cpp` is deleted, including the disk search for textures by filename
- [ ] `runtime_boot.cpp` registers no source importers in any runtime
- [ ] a missing cooked asset in the editor becomes a cook request plus a placeholder, and the asset swaps in when the cook finishes
- [ ] a test: an uncooked asset shows the placeholder, becomes ready after the cook, and there is no Assimp/cgltf call in the runtime process
- [ ] the asset looks the same whether or not it was cooked before the editor opened, because there is only one path now

## Contract
This is the flagship **real / null / job** contract. `asset-service::request(id)` returns a handle that is **Pending**, **Ready** or **Failed**, and every caller handles all three.

Nothing: **job**. Pending shows the placeholder asset of that type, never a null pointer.
- **real** (editor): Pending → cook request to CookService → Ready.
- **null** (player, server): no cooker in this build, so a missing cooked asset is **Failed** immediately with "not cooked: <path>", logged once, and the placeholder stays.
- **fake** (tests): Ready or Failed after N ticks, scripted.

Ownership: the handle is a value. The asset it resolves to belongs to the service; callers never free it.
