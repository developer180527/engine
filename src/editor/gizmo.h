#pragma once

#include <flecs.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <bx/math.h>
#include <GLFW/glfw3.h>
#include <cmath>

#include "engine_context.h"
#include "editor/gizmo_state.h"
#include "core/transform.h"
#include "inspector_panel.h"  // detail:: euler helpers

namespace gizmo_detail {

inline bx::Quaternion mtxToQuat(const float m[16]) {
    const float sx = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    const float sy = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
    const float sz = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

    const float invSx = sx > 1e-6f ? 1.0f/sx : 0.0f;
    const float invSy = sy > 1e-6f ? 1.0f/sy : 0.0f;
    const float invSz = sz > 1e-6f ? 1.0f/sz : 0.0f;

    const float r00 = m[0]*invSx, r01 = m[1]*invSx, r02 = m[2]*invSx;
    const float r10 = m[4]*invSy, r11 = m[5]*invSy, r12 = m[6]*invSy;
    const float r20 = m[8]*invSz, r21 = m[9]*invSz, r22 = m[10]*invSz;

    const float trace = r00 + r11 + r22;
    bx::Quaternion q{0,0,0,1};

    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (r12 - r21) * s;
        q.y = (r20 - r02) * s;
        q.z = (r01 - r10) * s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = 2.0f * std::sqrt(1.0f + r00 - r11 - r22);
        q.w = (r12 - r21) / s; q.x = 0.25f * s;
        q.y = (r01 + r10) / s; q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = 2.0f * std::sqrt(1.0f + r11 - r00 - r22);
        q.w = (r20 - r02) / s; q.x = (r01 + r10) / s;
        q.y = 0.25f * s;       q.z = (r12 + r21) / s;
    } else {
        const float s = 2.0f * std::sqrt(1.0f + r22 - r00 - r11);
        q.w = (r01 - r10) / s; q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s; q.z = 0.25f * s;
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
    return { m[12], m[13], m[14] };
}

} // namespace gizmo_detail

inline void gizmoBeginFrame() {
    ImGuizmo::BeginFrame();
    const ImGuiIO& io = ImGui::GetIO();
    ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
    ImGuizmo::SetOrthographic(false);
}

inline void gizmoHandleHotkeys(GLFWwindow* window, EngineContext& ctx) {
    if (ImGui::GetIO().WantTextInput) return;
    if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS)
        ctx.gizmoState.operation = ImGuizmo::TRANSLATE;
    if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS)
        ctx.gizmoState.operation = ImGuizmo::ROTATE;
    if (glfwGetKey(window, GLFW_KEY_Y) == GLFW_PRESS)
        ctx.gizmoState.operation = ImGuizmo::SCALE;
}

inline void drawGizmo(EngineContext& ctx,
                      const float view[16],
                      const float proj[16]) {
    if (!ctx.editor.selected.is_alive()) {
        ctx.gizmoState.lastSyncedFrom = flecs::entity{};
        return;
    }
    if (!ctx.editor.selected.has<Transform>()) return;

    Transform& t = ctx.editor.selected.get_mut<Transform>();

    const bool selectionChanged =
        (ctx.gizmoState.lastSyncedFrom != ctx.editor.selected);

    if (selectionChanged || !ImGuizmo::IsUsing()) {
        t.getMatrix(ctx.gizmoState.matrix);
        ctx.gizmoState.lastSyncedFrom = ctx.editor.selected;
    }

    ImGuizmo::Manipulate(view, proj,
                         ctx.gizmoState.operation,
                         ctx.gizmoState.mode,
                         ctx.gizmoState.matrix);

    if (ImGuizmo::IsUsing()) {
        switch (ctx.gizmoState.operation) {
            case ImGuizmo::TRANSLATE:
                t.position = gizmo_detail::mtxTranslation(ctx.gizmoState.matrix);
                break;
            case ImGuizmo::ROTATE:
                t.rotation = gizmo_detail::mtxToQuat(ctx.gizmoState.matrix);
                break;
            case ImGuizmo::SCALE:
                t.scale = gizmo_detail::mtxScale(ctx.gizmoState.matrix);
                break;
            default: break;
        }
    }
}
