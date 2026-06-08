#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class AnimProperty : uint8_t { Translation, Rotation, Scale };
enum class AnimInterp   : uint8_t { Step, Linear, CubicSpline };

struct AnimChannel {
    int           boneIndex = -1;
    AnimProperty  property  = AnimProperty::Translation;
    AnimInterp    interpolation = AnimInterp::Linear;

    std::vector<float> timestamps;
    // Packed flat: vec3 for Translation/Scale (3 floats per key),
    //              quat for Rotation (4 floats per key).
    // CubicSpline: in-tangent + value + out-tangent per key
    //              (3x the component count per key).
    std::vector<float> values;

    int keyCount() const { return (int)timestamps.size(); }

    int componentCount() const {
        return property == AnimProperty::Rotation ? 4 : 3;
    }

    int valuesPerKey() const {
        int cc = componentCount();
        return interpolation == AnimInterp::CubicSpline ? cc * 3 : cc;
    }
};

struct AnimClip {
    std::string              name;
    float                    duration       = 0.0f;
    float                    ticksPerSecond = 24.0f;
    std::vector<AnimChannel> channels;
};
