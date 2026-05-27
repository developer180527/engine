#pragma once
#include <flecs.h>
#include <imgui.h>
#include "engine_context.h"
#include "components/name.h"
#include "components/camera.h"
#include "components/rigid_body.h"
#include "core/transform.h"

// Forward-declared helper from inspector_panel
inline std::string uniqueEntityName(flecs::world& ecs, const std::string& base);

namespace detail_hier {

inline void spawnCamera(EngineContext& ctx) {
    std::string name = [&]{
        if (!ctx.ecs.lookup("Camera")) return std::string("Camera");
        for (int i = 2; i < 9999; ++i) {
            std::string c = "Camera (" + std::to_string(i) + ")";
            if (!ctx.ecs.lookup(c.c_str())) return c;
        }
        return std::string("Camera");
    }();
    Transform t;
    t.position = {0.0f, 5.0f, -10.0f};  // sensible default: above + behind origin
    ctx.editor.selected = ctx.ecs.entity(name.c_str())
        .set<Transform>(t)
        .set<Camera>({})
        .set<Name>({name});
    ctx.editor.sceneDirty = true;
}

// Shared add-entity popup — called from both button and right-click
inline void drawAddPopup(EngineContext& ctx, const char* popupId) {
    if (ImGui::BeginPopup(popupId)) {
        ImGui::TextDisabled("Add Entity");
        ImGui::Separator();

        if (ImGui::MenuItem("  [cam]  Camera"))
            spawnCamera(ctx);

        ImGui::Separator();
        ImGui::TextDisabled("Primitives (coming soon)");
        ImGui::BeginDisabled();
        ImGui::MenuItem("  Cube");
        ImGui::MenuItem("  Sphere");
        ImGui::MenuItem("  Plane");
        ImGui::MenuItem("  Torus");
        ImGui::MenuItem("  Capsule");
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::BeginDisabled();
        ImGui::MenuItem("  Empty Object");
        ImGui::MenuItem("  Point Light");
        ImGui::MenuItem("  Directional Light");
        ImGui::EndDisabled();

        ImGui::EndPopup();
    }
}

} // namespace detail_hier

inline void drawHierarchyPanel(EngineContext& ctx) {
    ImGui::Begin("Hierarchy");

    // ── Add button ────────────────────────────────────────────────────
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##addBtn");
    detail_hier::drawAddPopup(ctx, "##addBtn");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("%d entities", (int)ctx.ecs.count<Name>());

    ImGui::Separator();

    // ── Entity list ───────────────────────────────────────────────────
    flecs::entity toDelete{};

    ctx.ecs.query_builder<const Name>()
        .build()
        .each([&](flecs::entity e, const Name& n) {
            const bool isSelected = (ctx.editor.selected == e);
            ImGui::PushID(static_cast<int>(e.id()));

            // Icon prefix based on components
            const char* icon = e.has<Camera>() ? "[Cam] " : "";
            std::string label = std::string(icon) + n.value;

            if (ImGui::Selectable(label.c_str(), isSelected))
                ctx.editor.selected = e;

            // Per-entity right-click
            if (ImGui::BeginPopupContextItem("##entityCtx")) {
                ImGui::TextDisabled("%s", n.value.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) toDelete = e;
                if (ImGui::MenuItem("Select"))        ctx.editor.selected = e;
                ImGui::EndPopup();
            }

            ImGui::PopID();
        });

    // ── Delete key ────────────────────────────────────────────────────
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        ctx.editor.selected.is_alive())
        toDelete = ctx.editor.selected;

    if (toDelete.is_alive()) {
        if (ctx.editor.selected == toDelete) ctx.editor.selected = {};
        toDelete.destruct();
        ctx.editor.sceneDirty = true;
    }

    // ── Empty-area right-click → Add popup ───────────────────────────
    if (ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered())
        ImGui::OpenPopup("##addCtx");
    detail_hier::drawAddPopup(ctx, "##addCtx");

    ImGui::End();
}
