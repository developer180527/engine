#pragma once

#include <flecs.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <bx/math.h>
#include "editor/window_ops.h"
#include <cmath>

#include "editor/engine_context.h"
#include "editor/gizmo_state.h"
#include "core/transform.h"
#include "core/transform_utils.h"   // getWorldMatrix / safeInvert (parent chain)
#include <cstring>

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

inline void gizmoHandleHotkeys(edwin::WindowHandle window, EngineContext& ctx) {
    if (ctx.editor.playing()) return;          // editor authoring is inert in play
    if (ImGui::GetIO().WantTextInput) return;
    if (edwin::isKeyDown(window, Key::T))
        ctx.gizmoState.operation = ImGuizmo::TRANSLATE;
    if (edwin::isKeyDown(window, Key::R))
        ctx.gizmoState.operation = ImGuizmo::ROTATE;
    if (edwin::isKeyDown(window, Key::Y))
        ctx.gizmoState.operation = ImGuizmo::SCALE;
}

inline void drawGizmo(EngineContext& ctx,
                      const float view[16],
                      const float proj[16]) {
    // No editor authoring while playing — the gizmo would edit the editor
    // world behind the running snapshot. Drop the sync so it re-reads on Stop.
    if (ctx.editor.playing()) {
        ctx.gizmoState.lastSyncedFrom = flecs::entity{};
        return;
    }
    if (!ctx.editor.selected.is_alive()) {
        ctx.gizmoState.lastSyncedFrom = flecs::entity{};
        return;
    }
    if (!ctx.editor.selected.has<Transform>()) return;

    Transform& t = ctx.editor.selected.get_mut<Transform>();

    // The gizmo always works in WORLD space. For a parented entity the world
    // pose is parentWorld * local, so the handles sit on the object instead of
    // at its local coordinates (which is why the gun's gizmo was stranded near
    // the origin). Unparented => parentWorld is identity and this is a no-op.
    flecs::entity parent = ctx.editor.selected.target(flecs::ChildOf);
    const bool hasParent = parent && parent.is_alive() && parent.has<Transform>();
    float parentWorld[16]; bx::mtxIdentity(parentWorld);
    if (hasParent) getWorldMatrix(parent, parentWorld);

    const bool selectionChanged =
        (ctx.gizmoState.lastSyncedFrom != ctx.editor.selected);

    if (selectionChanged || !ImGuizmo::IsUsing()) {
        float localM[16]; t.getMatrix(localM);
        if (hasParent) bx::mtxMul(ctx.gizmoState.matrix, localM, parentWorld); // world = local * parent
        else           std::memcpy(ctx.gizmoState.matrix, localM, sizeof(localM));
        ctx.gizmoState.lastSyncedFrom = ctx.editor.selected;
    }

    ImGuizmo::Manipulate(view, proj,
                         ctx.gizmoState.operation,
                         ctx.gizmoState.mode,
                         ctx.gizmoState.matrix);

    const bool using_ = ImGuizmo::IsUsing();

    // Gizmo just started — capture transform for undo
    if (using_ && !ctx.gizmoState.wasUsing)
        ctx.gizmoState.transformBefore = t;

    if (using_) {
        // Bring the manipulated world matrix back to local before extracting.
        float local[16];
        if (hasParent) {
            float parentInv[16];
            safeInvert(parentInv, parentWorld);
            bx::mtxMul(local, ctx.gizmoState.matrix, parentInv);   // local = world * parent^-1
        } else {
            std::memcpy(local, ctx.gizmoState.matrix, sizeof(local));
        }
        switch (ctx.gizmoState.operation) {
            case ImGuizmo::TRANSLATE:
                t.position = gizmo_detail::mtxTranslation(local);
                break;
            case ImGuizmo::ROTATE:
                t.rotation = gizmo_detail::mtxToQuat(local);
                break;
            case ImGuizmo::SCALE:
                t.scale = gizmo_detail::mtxScale(local);
                break;
            default: break;
        }
    }

    // Gizmo just released — push undo with before/after transform
    if (!using_ && ctx.gizmoState.wasUsing) {
        ctx.editor.undoStack.pushTransform(
            ctx.editor.selected, ctx.gizmoState.transformBefore, t);
        ctx.editor.sceneDirty = true;
    }

    ctx.gizmoState.wasUsing = using_;
}
