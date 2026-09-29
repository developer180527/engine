#pragma once

#include <cstdint>
#include <flecs.h>
#include "core/transform.h"

// GizmoState lives in its own header so engine_context.h can include it
// without pulling in all of gizmo.h (which would create a circular dependency:
// engine_context.h -> gizmo.h -> inspector_panel.h -> engine_context.h).
//
// GUI-free: EngineContext includes this, and every panel receives an
// EngineContext, so an ImGuizmo type here put ImGui into every panel model's
// include graph. The gizmo's own vocabulary lives here; gizmo.h (the ImGuizmo
// front end) converts at the call.
enum class GizmoOp    : uint8_t { Translate, Rotate, Scale };
enum class GizmoSpace : uint8_t { Local, World };

struct GizmoState {
    GizmoOp    operation = GizmoOp::Translate;
    GizmoSpace mode      = GizmoSpace::Local;

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
