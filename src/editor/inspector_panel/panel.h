#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "editor/editor_icons.h"

// ── Component sections ──────────────────────────────────────────────────────
#include "editor/inspector_panel/utils.h"
#include "editor/inspector_panel/name_section.h"
#include "editor/inspector_panel/transform_section.h"
#include "editor/inspector_panel/spinner_section.h"
#include "editor/inspector_panel/mesh_material_section.h"
#include "editor/inspector_panel/camera_section.h"
#include "editor/inspector_panel/light_section.h"
#include "editor/inspector_panel/rigidbody_section.h"
#include "editor/inspector_panel/character_controller_section.h"
#include "editor/inspector_panel/script_section.h"
#include "editor/inspector_panel/animator_section.h"

inline void drawInspectorPanel(EngineContext& ctx) {
    ImGui::Begin(ICON_FA_WRENCH " Inspector");

    if (!ctx.editor.selected.is_alive()) {
        ImGui::TextDisabled("(no entity selected)");
        ImGui::End();
        return;
    }

    flecs::entity e = ctx.editor.selected;

    inspector_detail::drawNameSection(ctx, e);
    inspector_detail::drawParentSection(ctx, e);
    inspector_detail::drawTransformSection(ctx, e);
    inspector_detail::drawSpinnerSection(ctx, e);
    inspector_detail::drawMeshMaterialSection(ctx, e);
    inspector_detail::drawCameraSection(ctx, e);
    inspector_detail::drawLightSection(ctx, e);
    inspector_detail::drawRigidBodySection(ctx, e);
    inspector_detail::drawCharacterControllerSection(ctx, e);
    inspector_detail::drawScriptSection(ctx, e);
    inspector_detail::drawAnimatorSection(ctx, e);

    ImGui::End();
}
