#pragma once

#include <imgui.h>
#include <ImGuizmo.h>
#include <flecs.h>
#include "core/transform.h"

// GizmoState lives in its own header so engine_context.h can include it
// without pulling in all of gizmo.h (which would create a circular dependency:
// engine_context.h -> gizmo.h -> inspector_panel.h -> engine_context.h).
struct GizmoState {
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      mode      = ImGuizmo::LOCAL;

    float matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    flecs::entity lastSyncedFrom;

    // Undo tracking: capture transform when gizmo manipulation starts,
    // push undo command when it ends.
    bool      wasUsing       = false;
    Transform transformBefore;
};
