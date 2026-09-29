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
