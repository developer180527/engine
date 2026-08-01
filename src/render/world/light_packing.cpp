#include "render/world/light_packing.h"

#include <bx/math.h>

namespace rworld {

namespace {
void writeLight(float* b, LightType type, const Vec3& pos, const Vec3& dir,
                const Vec3& col, float intensity, float range,
                float innerCos, float outerCos) {
    b[0]  = pos.x; b[1]  = pos.y; b[2]  = pos.z; b[3]  = (float)(uint32_t)type;
    b[4]  = col.x; b[5]  = col.y; b[6]  = col.z; b[7]  = intensity;
    b[8]  = dir.x; b[9]  = dir.y; b[10] = dir.z; b[11] = range;
    b[12] = innerCos; b[13] = outerCos; b[14] = 0.0f; b[15] = 0.0f;
}
} // namespace

PackedLights packLights(const Span<LightItem>& lights, float ambient) {
    PackedLights out;
    out.ambient = ambient;

    if (lights.empty()) {
        // Default key light, identical to what the pipeline used to synthesize
        // inline. Preserved deliberately: an unlit scene reads as a broken
        // renderer, and the editor's default scene has no light entity.
        const bx::Vec3 d = bx::normalize(bx::Vec3{0.6f, 0.8f, 0.4f});
        writeLight(out.data, LightType::Directional,
                   Vec3{0.0f, 0.0f, 0.0f}, Vec3{d.x, d.y, d.z},
                   Vec3{1.0f, 0.98f, 0.92f}, 2.2f, 0.0f, 1.0f, 0.0f);
        out.count = 1;
        out.shadowLightIndex = 0;
        return out;
    }

    for (std::size_t i = 0; i < lights.size() && out.count < kMaxLights; ++i) {
        const LightItem& l = lights[i];
        writeLight(out.data + out.count * kFloatsPerLight, l.type, l.position,
                   l.direction, l.color, l.intensity, l.range,
                   l.spotInnerCos, l.spotOuterCos);
        // First shadow-casting light wins. Recorded by PACKED index, not by
        // source index: the two differ once lights are dropped at the cap, and
        // a shader indexing the wrong light shades from the wrong direction.
        if (l.castShadows && out.shadowLightIndex < 0)
            out.shadowLightIndex = out.count;
        ++out.count;
    }
    return out;
}

} // namespace rworld
