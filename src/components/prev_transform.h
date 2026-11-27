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

// Whether the history equals the pose BITWISE, so that interpolating between
// them gives the same matrix at every alpha. Used where a write is marked only
// when the pose really moves (WO-072): prevSnapshot, and the retained render
// scene deciding which rows still interpolate. Bitwise, not ==: with ==, a NaN
// never equals itself and the row would be reported every step forever; with
// bitwise, -0.0 against 0.0 reports one row that did not really move, which is
// the harmless direction.
#include <cstring>
#include "core/transform.h"
inline bool samePose(const Transform& t, const PrevTransform& p) {
    return std::memcmp(&t.position, &p.position, sizeof p.position) == 0
        && std::memcmp(&t.rotation, &p.rotation, sizeof p.rotation) == 0
        && std::memcmp(&t.scale,    &p.scale,    sizeof p.scale)    == 0;
}
