#pragma once
// ── PrevTransform — last sim tick's transform (render interpolation) ────────
// Written by the runtime at the START of every fixed sim step; the renderer
// lerps PrevTransform -> Transform by the accumulator fraction so motion is
// smooth at ANY frame rate vs the 60Hz sim. ENGINE-INTERNAL: never serialized,
// never reflected, absent on render-rate-driven entities (cameras — their
// rotation is late-latched in onFrame and must not be dragged back by a lerp).
#include <bx/math.h>

struct PrevTransform {
    bx::Vec3       position = bx::InitZero;
    bx::Quaternion rotation = bx::InitIdentity;
    bx::Vec3       scale    = bx::InitZero;
};
