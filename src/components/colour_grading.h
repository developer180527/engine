#pragma once
// ── ColourGrading — how a camera turns light into a picture ─────────────────
//
// Colour pipeline stage B (docs/plans/colour-pipeline.md §4). Exposure, the tone
// mapper and an optional grading LUT, for the camera entity it sits on. The
// primary camera's grading drives the game view and the standalone player; the
// editor's own scene view keeps the defaults.
//
// ── A SEPARATE COMPONENT, NOT FIELDS ON Camera ─────────────────────────────
// Camera is in engine_abi::componentLayoutHash — a field added there would make
// every kit's module refuse to load. This is presentation data nothing in a kit
// reads, so it does not belong in that frozen surface; it sits beside Camera on
// the same entity instead. Not re-exported by include/engine/components.h, and
// deliberately not in the hash.
//
// ── Presentation ────────────────────────────────────────────────────────────
// SimExempt (runtime/sim_classification.cpp): it decides what the picture looks
// like, and nothing in the simulation reads it. Serialized by the hand-written
// scene serde, because lutPath is a std::string flecs meta cannot carry.
//
// ── DEFAULTS PRESERVE AN UNGRADED SCENE ─────────────────────────────────────
// Manual exposure at 0 stops is a gain of exactly 1, which is why Manual is the
// default and not Physical: the engine's lights are not photometric (a sun of
// intensity 3, not 100 000 lux), and physical exposure at any real camera
// setting renders such a scene black. Physical mode is here for when lights
// gain units — and for the film tool, whose cameras are specified this way.
#include <cmath>
#include <cstdint>
#include <string>

#include "core/display_transform.h"

struct ColourGrading {
    // display::ExposureMode / display::ToneMapper, stored as bytes so flecs meta
    // and the scene file see plain numbers. Unknown values fall back to the
    // defaults in resolved*() below rather than reaching the shader.
    uint8_t     exposureMode   = (uint8_t)display::ExposureMode::Manual;
    uint8_t     toneMapper     = (uint8_t)display::ToneMapper::PbrNeutral;
    float       exposureEV     = 0.0f;    // stops of gain in BOTH modes; +1 = 2x
    float       aperture       = 16.0f;   // f-number     ┐
    float       shutterSeconds = 0.01f;   // seconds      ├ Physical mode only
    float       iso            = 100.0f;  // sensitivity  ┘
    std::string lutPath;                  // project-relative .cube; empty = none
};

// The linear scale the output pass multiplies scene radiance by. Never NaN or
// infinite: a scene file can hold anything, and a non-finite exposure would turn
// the whole view into one colour.
inline float resolvedExposure(const ColourGrading& g) {
    const float ev = std::isfinite(g.exposureEV) ? g.exposureEV : 0.0f;
    float e = display::exposureGain(ev);
    const bool physicalValid =
        g.exposureMode == (uint8_t)display::ExposureMode::Physical &&
        std::isfinite(g.aperture) && std::isfinite(g.shutterSeconds) &&
        std::isfinite(g.iso) && g.aperture > 0.0f && g.shutterSeconds > 0.0f &&
        g.iso > 0.0f;
    if (physicalValid)
        e *= display::exposureFromEv100(
            display::ev100(g.aperture, g.shutterSeconds, g.iso));
    return (std::isfinite(e) && e > 0.0f) ? e : 1.0f;
}

inline display::ToneMapper resolvedToneMapper(const ColourGrading& g) {
    return g.toneMapper < (uint8_t)display::ToneMapper::Count
        ? (display::ToneMapper)g.toneMapper
        : display::ToneMapper::PbrNeutral;
}
