#pragma once
#include <cstdint>

enum class ProjectionType { Perspective, Orthographic };

struct Camera {
    // The MEANING of this component in the kit ABI (WO-051). Bump it when the same
    // bytes start to mean something else, and add a note to docs/guides/kit-abi-revisions.md.
    static constexpr uint32_t kAbiRevision = 0;

    ProjectionType projection  = ProjectionType::Perspective;
    float          fov         = 60.0f;    // degrees, perspective only
    float          orthoSize   = 10.0f;    // world units, ortho only
    float          nearPlane   = 0.1f;
    float          farPlane    = 1000.0f;
    float          clearColor[4] = {0.1f, 0.1f, 0.12f, 1.0f};
    bool           isPrimary   = true;     // game view renders from primary camera
};
