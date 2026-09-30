---
status: as-built
contract: cook-request
kind: interface
state: provisional
owner: src/assets
header: src/assets/cook_requests.h
implementations:
  - real: src/assets/cookers/cook_service.h#CookService
  - null: src/assets/cook_requests.h#NullCookRequests
  - fake: tests/asset_cook_request_test.cpp#FakeCook
tests:
  - tests/asset_cook_request_test.cpp
covers:
  - src/assets/cook_requests.h
  - src/runtime/services/async_loader.h
verified: 2026-10-01
---

# cook-request — a missing cooked asset is a job

The runtime loads cooked content only (WO-018). When something asks for an
asset by its SOURCE path (`AsyncLoader::load`, which the editor's spawn and
scene loading use through `SceneAssets::streamMesh`) and the registry has no
Ready cook of it, `ICookRequests` decides what happens. The loader never
parses a source format itself: that was a second parser beside the cook's,
and it made an asset look different depending on whether it had been cooked.

A request's lifetime, as `AsyncLoader::state(path)` reports it:
**Pending** → **Ready** (the cook landed, the cooked asset loaded) or
**Failed** (why is in `AsyncLoadResult::error`). A scene entity waiting on
one shows the placeholder mesh (`MeshPlaceholder`), never nothing.

## Nothing
The null provider (`NullCookRequests`, or no provider set) is a build with no
cooker: the player, a server, `engine_host`, `scene_resave`. A missing cook
is **Failed at once**, "not cooked: <path> (this build has no cooker; run
engine_cook)", logged once per path, and a scene entity keeps the placeholder
and its authored reference (so a save writes it back unchanged).

## Ownership
The provider is not owned by the loader (`setCookRequests` takes a pointer)
and must outlive its pending requests; the editor's `CookService` lives for
the session. Handles returned on Ready belong to the runtime registries, as
any loaded asset's do; callers never free them.

## Threading
`requestCook` may be called from any thread and must not block;
`CookService` records the path and wakes its cook thread. The loader calls it
from the main thread (`drainOne`). Completion is observed through the asset
registry (the record becomes Ready or Failed), read only by the loader's
worker job, which keeps registry access single-consumer.

## Timing
Asynchronous. The loader asks once per source path, then retries the load on
its worker every `AsyncLoader::kRetryDrains` drains (~0.5 s at one drain a
frame). `CookService` puts a requested asset in scope and at the front of the
next pass. A cooked mesh whose materials name uncooked textures waits for
those too, so it never appears untextured and then changes. A request that
has not finished after ~5 minutes of retries is Failed ("cook requested but
never finished").

## Errors
Failed carries the reason in `AsyncLoadResult::error`: "not cooked" (null
provider), "cook failed: <path> — <cook's message>" (the registry record is
Failed), an unreadable or stale cooked file (stride mismatch: re-cook), or
the timeout above. Each is logged once per path. A texture whose cook failed
does not fail the mesh: the mesh loads without it, with a warning.
