#pragma once
// ── AnimClip — a runtime animation clip (ozz backbone) ───────────────────────
// The clip IS an ozz::animation::Animation: compressed, SoA-friendly keyframes
// built at import by anim::buildOzzClip from Assimp data, bound to a specific
// skeleton's joint order. Sampling goes through ozz SamplingJob (see
// systems/animator_system.h). The old hand-rolled AnimChannel storage and
// sampler are gone — "don't reinvent the wheel" (Jolt/bgfx/flecs precedent).
#include <memory>
#include <string>

namespace ozz::animation { class Animation; }

struct AnimClip {
    std::string name;
    float       duration = 0.0f;     // seconds (mirrors ozz->duration())

    // Immutable shared clip data; shared_ptr keeps AnimClip copyable for the
    // registry. Null = empty/invalid clip.
    std::shared_ptr<const ozz::animation::Animation> ozz;

    // Import diagnostics: how many source tracks mapped onto the skeleton.
    int mappedTracks = 0;
    int totalTracks  = 0;

    bool valid() const { return ozz != nullptr; }
};
