#pragma once
#include "core/handle.h"
#include "render/world/lod.h"   // kMaxLodLevels, and the selection contract

// LodMesh — the COARSER levels of a mesh, and when to switch to them.
//
// Level 0 is `MeshRenderer::mesh`; this component holds levels 1..count. An entity
// with no LodMesh renders at full detail, and deleting the component is a valid way
// to say "always full detail" — see render/world/lod.h for why level 0 is deliberately
// not described here.
//
// `coarsenBelow[i]` is the fraction of the VIEWPORT HEIGHT below which level i+1
// replaces level i, so the array descends. The defaults are a starting point for
// human-scale props, not a measurement: a 2 m prop covering less than ~30% of the
// screen height drops to level 1, ~10% to level 2, ~3% to level 3.
//
// Fixed-size on purpose. This is read inside the parallel extraction loop, where a
// heap-allocated level list would put an indirection — and a lifetime question — on
// the hot path for the sake of a fifth level nobody authors.
struct LodMesh {
    MeshHandle mesh[rworld::kMaxLodLevels - 1] = {};
    float      coarsenBelow[rworld::kMaxLodLevels - 1] = { 0.30f, 0.10f, 0.03f };
    // How many entries of `mesh` are real. 0 = inert (renders exactly like no
    // component at all), which is what a freshly added component should do rather
    // than silently swapping in a null mesh.
    uint8_t    count = 0;
};
