#pragma once
#include "core/math_types.h"
#include <cstdint>
#include <cmath>

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
    bool      useTemperature = false; // author color via Kelvin instead of RGB
    float     temperatureK   = 6500.0f; // 1500..15000; effective = kelvin * color
};

// Blackbody color temperature (Kelvin) -> approx sRGB [0,1] (Tanner Helland).
// CPU-side only; the bake happens once per light at extraction, never per-pixel.
inline Vec3 kelvinToRGB(float kelvin) {
    const float t = kelvin / 100.0f;
    float r, g, b;
    if (t <= 66.0f) { r = 255.0f;                                       g = 99.4708025861f * std::log(t) - 161.1195681661f; }
    else            { r = 329.698727446f  * std::pow(t - 60.0f, -0.1332047592f);
                      g = 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f); }
    if      (t >= 66.0f) b = 255.0f;
    else if (t <= 19.0f) b = 0.0f;
    else                 b = 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f;
    auto c01 = [](float v){ v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); return v / 255.0f; };
    return Vec3{ c01(r), c01(g), c01(b) };
}
