#pragma once

#include <flecs.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <bx/math.h>
#include <GLFW/glfw3.h>
#include <cmath>

#include "editor_state.h"
#include "core/transform.h"

// Gizmo subsystem.
//
// Renders a 3D manipulation gizmo over the scene for the currently selected
// entity. Translate/rotate/scale modes; drag to manipulate.
//
// Two design rules govern this code, both learned the hard way:
//
// RULE 1: the matrix passed to ImGuizmo MUST be built the same way the
// renderer builds the model matrix. We use `t.getMatrix()` (bgfx convention,
// row-major SRT). If we used a different builder (like ImGuizmo's
// RecomposeMatrixFromComponents), the gizmo would draw at one world position
// and the cube at another — same Transform, different conventions.
//
// RULE 2: the matrix is cached in GizmoState between frames. ImGuizmo
// computes drag deltas by comparing matrix-now vs matrix-at-drag-start.
// If we rebuild the matrix from Transform each frame, even the smallest
// quat<->matrix round-trip drift looks like an external rotation to
// ImGuizmo and gets amplified into wild output. The cache makes ImGuizmo
// the authoritative owner of the matrix during a drag.
//
// Sync from Transform → cache happens only when (a) selection changes,
// or (b) the user is not currently dragging.

namespace gizmo_detail {

// Extract a quaternion from the rotation portion of a row-major bgfx matrix.
//
// The matrix may include scale (rows scaled by sx/sy/sz). We normalize each
// row to remove scale, then apply the inverse of bx::mtxFromQuaternion.
//
// The element formulas were derived directly from bx::mtxFromQuaternion's
// source — going through Shepperd's standard formula but using bgfx's
// specific row/column choices. This is the only conversion path that
// round-trips cleanly: bx::mtxFromQuaternion(q) -> mtxToQuat() == q.
inline bx::Quaternion mtxToQuat(const float m[16]) {
    // Per-row lengths give per-axis scale (since t.getMatrix builds S*RT,
    // each row of the upper-left 3x3 = scale[i] * row_of_pure_rotation[i]).
    const float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    const float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    const float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

    const float invSx = (sx > 1e-6f) ? 1.0f / sx : 0.0f;
    const float invSy = (sy > 1e-6f) ? 1.0f / sy : 0.0f;
    const float invSz = (sz > 1e-6f) ? 1.0f / sz : 0.0f;

    // Pure rotation 3x3 (rows divided by per-row scale).
    const float r00 = m[0]  * invSx, r01 = m[1]  * invSx, r02 = m[2]  * invSx;
    const float r10 = m[4]  * invSy, r11 = m[5]  * invSy, r12 = m[6]  * invSy;
    const float r20 = m[8]  * invSz, r21 = m[9]  * invSz, r22 = m[10] * invSz;

    const float trace = r00 + r11 + r22;
    bx::Quaternion q { 0, 0, 0, 1 };

    // Pick the largest principal element for numerical stability.
    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (r12 - r21) * s;
        q.y = (r20 - r02) * s;
        q.z = (r01 - r10) * s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = 2.0f * std::sqrt(1.0f + r00 - r11 - r22);
        q.w = (r12 - r21) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = 2.0f * std::sqrt(1.0f + r11 - r00 - r22);
        q.w = (r20 - r02) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + r22 - r00 - r11);
        q.w = (r01 - r10) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }

    return bx::normalize(q);
}

inline bx::Vec3 mtxScale(const float m[16]) {
    return {
        std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]),
        std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]),
        std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]),
    };
}

inline bx::Vec3 mtxTranslation(const float m[16]) {
    // Row-major, row-vector convention: translation lives in the last row.
    return { m[12], m[13], m[14] };
}

} // namespace gizmo_detail

struct GizmoState {
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      mode      = ImGuizmo::LOCAL;

    // Cached matrix in bgfx convention (built via t.getMatrix). See file
    // header for why we cache.
    float matrix[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    flecs::entity lastSyncedFrom;
};

inline void gizmoBeginFrame() {
    ImGuizmo::BeginFrame();
    const ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(false);
}

inline void gizmoHandleHotkeys(GLFWwindow* window, GizmoState& state) {
    if (ImGui::GetIO().WantTextInput) return;

    if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) state.operation = ImGuizmo::TRANSLATE;
    if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) state.operation = ImGuizmo::ROTATE;
    if (glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS) state.operation = ImGuizmo::SCALE;
}

inline void drawGizmo(EditorState& editor,
                      const float view[16],
                      const float proj[16],
                      GizmoState& state) {
    if (!editor.selected.is_alive()) {
        state.lastSyncedFrom = flecs::entity{};
        return;
    }
    if (!editor.selected.has<Transform>()) return;

    Transform& t = editor.selected.get_mut<Transform>();

    // Sync from Transform when not dragging or when selection changed.
    // Use t.getMatrix — the same builder the renderer uses — so the gizmo's
    // visual position matches the cube's rendered position exactly.
    const bool selectionChanged = (state.lastSyncedFrom != editor.selected);
    if (selectionChanged || !ImGuizmo::IsUsing()) {
        t.getMatrix(state.matrix);
        state.lastSyncedFrom = editor.selected;
    }

    ImGuizmo::Manipulate(
        view, proj,
        state.operation,
        state.mode,
        state.matrix
    );

    // Write back ONLY while the user is actively dragging.
    //
    // Without this gate, a click on the gizmo (without drag) registers as
    // changed=true because ImGuizmo touches the matrix on click, even if
    // the user never moved the mouse. Reading back from that touched matrix
    // and writing to t can introduce a one-frame jump from accumulated
    // float precision. IsUsing() is true only while the user holds the
    // gizmo — the right window for syncing matrix -> Transform.
    if (ImGuizmo::IsUsing()) {
        // Extract the modified field directly from the matrix using bgfx
        // conventions (no Euler intermediate). Each extractor is the inverse
        // of the corresponding step in t.getMatrix(), so writing back and
        // re-deriving via t.getMatrix() round-trips cleanly.
        switch (state.operation) {
            case ImGuizmo::TRANSLATE:
                t.position = gizmo_detail::mtxTranslation(state.matrix);
                break;

            case ImGuizmo::ROTATE:
                t.rotation = gizmo_detail::mtxToQuat(state.matrix);
                break;

            case ImGuizmo::SCALE:
                t.scale = gizmo_detail::mtxScale(state.matrix);
                break;

            default:
                break;
        }
    }
}
