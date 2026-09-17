#pragma once
// ── DisplayTransform — what the output pass does to one view ────────────────
//
// Colour pipeline stage B. Resolved from a camera's ColourGrading by the runtime
// (runtime/services/lut_library.h) and handed to the renderer, which applies it
// in the output pass: exposure, tone map, sRGB encode, grade. Backend-free, so it
// can cross IRenderer.
//
// The defaults are what a view with no grading gets: gain 1, PBR Neutral, no LUT.
#include <cstdint>
#include <memory>

#include "core/display_transform.h"

struct CubeLut;   // core/cube_lut.h

// The renderer's three display outputs, each with its own transform.
enum class DisplayView : uint8_t {
    Scene      = 0,   // the editor's scene viewport
    Game       = 1,   // the editor's game view
    Backbuffer = 2,   // the standalone player
    Count
};

struct DisplayTransform {
    float                          exposure   = 1.0f;   // linear, already resolved
    display::ToneMapper            toneMapper = display::ToneMapper::PbrNeutral;
    std::shared_ptr<const CubeLut> lut;                 // null = no grade
};
