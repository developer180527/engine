#pragma once
// ── lod_limit — how many LOD levels a mesh may have, stated once ──────────────
//
// Four levels TOTAL: level 0 plus three coarser. Three is what most kits ship
// (near, far, billboard), and the fourth leaves room for an impostor without
// making the component variable-length, which would put an allocation in the
// extraction path. The selection rules are render/world/lod.h's.
//
// Here, in core, because two layers need it: the LodMesh component sizes its
// chain by it, and the GPU-free render world selects levels against it. It
// lived in render/world/lod.h, and the component including that header made
// components depend on render, one of the edges that closed the module cycle
// (WO-046). Dependency-free, like core/bone_limit.h.
#include <cstdint>

inline constexpr uint8_t kMaxLodLevels = 4;
