#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "core/transform.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawTransformSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Transform>()) return;

    sectionHeader("Transform");
    Transform& t = e.get_mut<Transform>();

    // Transform-specific undo capture (separate from propEdit because
    // Transform uses the lightweight pushTransform path, not the JSON-based
    // pushPropertyEdit).
    static Transform s_tfBefore;
    static bool      s_tfCapturing = false;
    auto onTfActivate = [&]{
        if (!s_tfCapturing && ImGui::IsItemActivated()) {
            if (const Transform* pt = e.try_get<Transform>()) s_tfBefore = *pt;
            s_tfCapturing = true;
        }
    };
    auto onTfCommit = [&]{
        if (s_tfCapturing && ImGui::IsItemDeactivatedAfterEdit()) {
            if (const Transform* pt = e.try_get<Transform>())
                ctx.editor.undoStack.pushTransform(e, s_tfBefore, *pt);
            s_tfCapturing = false;
        } else if (s_tfCapturing && ImGui::IsItemDeactivated()) {
            s_tfCapturing = false;
        }
    };

    ImGui::DragFloat3("Position", &t.position.x, 0.05f);
    onTfActivate(); onTfCommit();
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

    bx::Vec3 eulerDeg = quatToEulerDeg(t.rotation);
    if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f))
        t.rotation = eulerDegToQuat(eulerDeg);
    onTfActivate(); onTfCommit();
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

    ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
    onTfActivate(); onTfCommit();
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
}

} // namespace inspector_detail
