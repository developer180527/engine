---
status: target
contract: asset-service
kind: interface
state: planned
owner: src/runtime/services
header: 
implementations:
  - real: src/runtime/services/asset_service.h#AssetService
---

# asset-service — planned

Reached today as the CONCRETE class (`AssetService`) from the runtime,
scripting, scene loading, the C++ SDK and the editor — the widest concrete
surface in the engine (`docs/plans/subsystem-contracts.md` §3.2). Planned: an
interface, a null, and a fake, so anything that loads assets can be built and
tested without the real service.

**What WO-018 settled, and where it lives.** The real/null/job behaviour for
an asset that is not cooked yet is NOT on this service: `AssetService` takes
COOKED paths, and a cooked path that does not exist is simply a failure
(`loadFailed`). "Not cooked yet" can only arise when something asks by SOURCE
path, which is `AsyncLoader` (Pending → Ready/Failed, `state(path)`), and the
job behind it is its own contract, `cook-request` (`docs/contracts/cook-request.md`:
real `CookService`, null `NullCookRequests`, fake in the test). This contract
stays planned for the interface/null/fake split of `AssetService` itself.

## Nothing
Not yet written.

## Ownership
Not yet written.

## Threading
Not yet written.

## Timing
Not yet written.

## Errors
Not yet written.
