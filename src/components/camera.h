#pragma once

enum class ProjectionType { Perspective, Orthographic };

struct Camera {
    ProjectionType projection  = ProjectionType::Perspective;
    float          fov         = 60.0f;    // degrees, perspective only
    float          orthoSize   = 10.0f;    // world units, ortho only
    float          nearPlane   = 0.1f;
    float          farPlane    = 1000.0f;
    float          clearColor[4] = {0.1f, 0.1f, 0.12f, 1.0f};
    bool           isPrimary   = true;     // game view renders from primary camera
};
