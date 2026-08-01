#pragma once
// ── Light packing — LightItem[] to a GPU-ready float array ──────────────────
//
// ONE concern: lay lights out the way the shader expects.
//
// This was ~30 lines inline in ForwardPipeline::render, mixing "what lights
// exist" with "how do I upload them" — so a project replacing the pipeline
// inherited neither, and the packing layout was invisible to any test.
//
// GPU-free: it fills a plain float buffer. The caller hands that to bgfx.
#include "render/world/render_world.h"

#include <cstdint>

namespace rworld {

// Fixed forward-lighting budget. Lights beyond this are DROPPED — the honest
// limitation of a plain forward path, and the reason clustered forward is the
// stated target (docs/renderer-architecture.md §2).
constexpr int kMaxLights   = 16;
constexpr int kFloatsPerLight = 16;   // 4 vec4s

struct PackedLights {
    // [pos.xyz, type][color.rgb, intensity][dir.xyz, range][inner, outer, _, _]
    float  data[kMaxLights * kFloatsPerLight] = {};
    int    count   = 0;
    float  ambient = 0.0f;
    // Index of the light casting the shadow map (-1 = none). The shadow pass
    // and the lighting shader must agree on WHICH light is shadowed; passing
    // it explicitly beats both sides re-deriving it and disagreeing.
    int    shadowLightIndex = -1;

    int vec4Count() const { return count * 4; }   // what setUniform() wants
};

// Pack up to kMaxLights. When `lights` is empty a default key light is
// synthesized, matching the previous behaviour — a scene with no lights
// rendering pure black looks like a broken renderer, not an empty scene.
PackedLights packLights(const Span<LightItem>& lights, float ambient);

} // namespace rworld
