---
status: target
contract: scene-service
kind: interface
state: planned
owner: src/runtime/services
header: 
implementations:
  - real: src/runtime/services/scene_service.h#SceneService
---

# scene-service — planned

Reached today as the CONCRETE class (`SceneService`) from the runtime,
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
**Loading never deletes authored data.** A reference the loader cannot resolve
stays in the document and is reported: it is kept on the entity, written back
unchanged by the next save, and shown in the editor with its reason. A failed
load is a warning in the log, never a silent drop. (WO-029, pinned by
`tests/scene_mesh_reference_test.cpp` for the JSON path, `scene_serializer.h`.)

Gap: the cooked binary path, `SceneService::load`, still fails a mesh load
silently. It is read-only, so nothing is lost, but nothing is reported either.
