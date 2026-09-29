#pragma once

#include <imgui.h>
#include <flecs.h>

#include "editor/engine_context.h"
#include "core/transform.h"
#include "editor/panels/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawTransformSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Transform>()) return;

    sectionHeader("Transform");
    Transform& t = e.get_mut<Transform>();

    // One drag = one undo step (inspector model's TransformEdit), driven by
    // ImGui's activation signals.
    static inspect::TransformEdit s_edit;
    auto track = [&]{
        s_edit.track(ctx, e, ImGui::IsItemActivated(),
                     ImGui::IsItemDeactivatedAfterEdit(), ImGui::IsItemDeactivated());
    };

    ImGui::DragFloat3("Position", &t.position.x, 0.05f);
    track();

    bx::Vec3 eulerDeg = quatToEulerDeg(t.rotation);
    if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f))
        t.rotation = eulerDegToQuat(eulerDeg);
    track();

    ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
    track();
}

} // namespace inspector_detail
