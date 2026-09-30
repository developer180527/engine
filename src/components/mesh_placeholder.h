#pragma once
// ── MeshPlaceholder — what an entity SHOWS while its own mesh is not there ─────
//
// WO-018: the runtime loads cooked content only, so an entity whose mesh has
// not been cooked yet waits for a cook (Pending) or, in a build with no cooker,
// never gets it (Failed). Either way it shows the placeholder mesh, never
// nothing, and the authored reference stays in UnresolvedMesh (scene/).
//
// `shown` is the handle the entity's MeshRenderer was given AS the stand-in.
// The serializer writes the authored reference instead of the MeshRenderer
// exactly while the renderer still holds `shown`: if anyone assigns a real
// mesh, the handles differ and the real one is what gets saved.
//
// Not simulation state (SimExempt, runtime/sim_classification.cpp): it is a
// loading artifact, like UnresolvedMesh, and is never written to disk.
#include "core/handle.h"

struct MeshPlaceholder {
    MeshHandle shown;
};
