#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/spinner.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawSpinnerSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Spinner>()) return;
    sectionHeader("Spinner");
    Spinner& s = e.get_mut<Spinner>();
    ImGui::DragFloat("Yaw speed",   &s.speedYaw,   0.05f);
    ImGui::DragFloat("Pitch speed", &s.speedPitch, 0.05f);
}

} // namespace inspector_detail
