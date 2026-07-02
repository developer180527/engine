#pragma once

#include <imgui.h>
#include <flecs.h>

#include "editor/engine_context.h"
#include "editor/editor_icons.h"

// ── Component sections ──────────────────────────────────────────────────────
#include "editor/panels/inspector_panel/utils.h"
#include "editor/panels/inspector_panel/name_section.h"
#include "editor/panels/inspector_panel/transform_section.h"
#include "editor/panels/inspector_panel/spinner_section.h"
#include "editor/panels/inspector_panel/mesh_material_section.h"
#include "editor/panels/inspector_panel/camera_section.h"
#include "editor/panels/inspector_panel/light_section.h"
#include "editor/panels/inspector_panel/rigidbody_section.h"
#include "editor/panels/inspector_panel/character_controller_section.h"
#include "editor/panels/inspector_panel/script_section.h"
#include "editor/panels/inspector_panel/animator_section.h"
#include "editor/panels/inspector_panel/reflected_section.h"
#include "editor/panels/inspector_panel/add_component.h"

inline void drawInspectorPanel(EngineContext& ctx, bool* open) {
    if (open && !*open) return;
    ImGui::Begin(ICON_FA_WRENCH " Inspector", open);

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
    inspector_detail::drawReflectedSections(ctx, e);   // meta-driven (kit comps)

    inspector_detail::drawAddComponentButton(ctx, e);

    ImGui::End();
}
