#pragma once
#include "core/math_types.h"
#include <cstdint>

// A light's PLACEMENT/ORIENTATION come from the entity's Transform:
//   position  = world translation
//   direction = transform's forward axis (directional/spot)
// This component carries only the light's PARAMETERS; the renderer derives
// direction/position from the world matrix at extraction time.
// TODO (Jun 3, 04:00 PM):
// - Keep Transform as the single source of truth for position/orientation.
// - Renderer should derive world position and direction during extraction.
// - Avoid storing duplicated position/direction inside Light.
// - Consider packing LightType/castShadows more tightly if light counts become very large.
// - Consider adding color-temperature support (e.g. 2700K/6500K) alongside RGB color.
// - Runtime extraction should build render-light data from Transform + Light components.
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

struct Light {
    LightType type      = LightType::Directional;
    Vec3      color     { 1.0f, 1.0f, 1.0f };
    float     intensity = 3.0f;    // ~ the old hardcoded sun
    float     range     = 15.0f;   // point/spot falloff (world units)
    float     spotInner = 25.0f;   // inner cone half-angle (deg), spot
    float     spotOuter = 35.0f;   // outer cone half-angle (deg), spot
    bool      castShadows = false; // honoured once the shadow pass lands
};
