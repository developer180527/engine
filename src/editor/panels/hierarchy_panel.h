#pragma once
#include "components/light.h"
#include <imgui.h>
#include "editor/editor_icons.h"
#include <flecs.h>
#include <string>
#include "editor/engine_context.h"
#include "components/name.h"
#include "components/camera.h"
#include "components/spinner.h"
#include "components/rigid_body.h"
#include "render/primitive_library.h"
#include "core/transform.h"
#include "editor/panels/asset_browser/spawn.h"
#include "components/transform_hierarchy.h"
#include "components/entity_id_util.h"
#include <cstring>

// Hierarchy panel — the ImGui front end of hier::HierarchyModel
// (panels/hierarchy/model.h). What is listed and what add / delete / reparent
// do lives in the model; this file draws the tree, menus and drag-and-drop.
#include "editor/panels/hierarchy/model.h"

namespace detail_hier {

// ── Add-entity menu items (call from any popup context) ───────────────────
inline void drawAddMenuItems(EngineContext& ctx, hier::HierarchyModel& model) {
    const char* group = nullptr;
    for (const hier::AddItem& it : hier::addItems()) {
        if (!group || std::strcmp(group, it.group) != 0) {
            if (group) ImGui::Separator();
            ImGui::TextDisabled("%s", it.group);
            if (!group) ImGui::Separator();
            group = it.group;
        }
        const bool ok = model.canAdd(ctx, it.kind);
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::MenuItem(it.label) && ok) model.add(ctx, it.kind);
        if (!ok) ImGui::EndDisabled();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Physics");
    if (ctx.editor.selected.is_alive() && ImGui::MenuItem("RigidBody (add to selected)"))
        model.addRigidBodyToSelected(ctx);
}

// Accept an ENTITY_ID drop on the last item and request the reparent.
inline void acceptEntityDrop(EngineContext& ctx, hier::HierarchyModel& model,
                             flecs::entity newParent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (auto* pl = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
        flecs::entity dragged = ctx.ecs.entity(*(const flecs::entity_t*)pl->Data);
        model.requestReparent(dragged, newParent);   // refuses cycles itself
    }
    ImGui::EndDragDropTarget();
}

// ── Recursive entity tree node ─────────────────────────────────────────────
inline void drawEntityNode(flecs::entity e, EngineContext& ctx, hier::HierarchyModel& model) {
    const std::string label = hier::label(e);
    const bool hasKids = hier::hasChildren(e);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasKids)                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (ctx.editor.selected == e) flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID((int)(uint32_t)e.id());
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        ctx.editor.selected = e;

    if (ImGui::BeginDragDropSource()) {
        flecs::entity_t id = e.id();
        ImGui::SetDragDropPayload("ENTITY_ID", &id, sizeof(id));
        ImGui::Text("  %s", label.c_str());
        ImGui::EndDragDropSource();
    }
    acceptEntityDrop(ctx, model, e);

    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("Select")) ctx.editor.selected = e;
        if (!hier::isRoot(e) && ImGui::MenuItem("Unparent")) model.requestUnparent(e);
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, {1, 0.3f, 0.3f, 1});
        if (ImGui::MenuItem("Delete")) model.requestDelete(e);
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    if (open && hasKids) {
        hier::forEachChild(e, [&](flecs::entity c) { drawEntityNode(c, ctx, model); });
        ImGui::TreePop();
    }
    ImGui::PopID(); // always last — after TreePop for open nodes, safe for leaves
}

} // namespace detail_hier

// ── drawHierarchyPanel ─────────────────────────────────────────────────────
inline void drawHierarchyPanel(EngineContext& ctx, bool* open) {
    if (open && !*open) return;
    static hier::HierarchyModel s_model;
    ImGui::Begin(ICON_FA_SITEMAP " Hierarchy", open);

    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##addEntity");
    ImGui::SameLine();
    ImGui::TextDisabled("%d entities", hier::entityCount(ctx.ecs));

    if (ImGui::BeginPopup("##addEntity")) {
        detail_hier::drawAddMenuItems(ctx, s_model);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupContextWindow("##hierCtx",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        detail_hier::drawAddMenuItems(ctx, s_model);
        ImGui::EndPopup();
    }

    ImGui::Separator();

    hier::forEachRoot(ctx.ecs, [&](flecs::entity e) { detail_hier::drawEntityNode(e, ctx, s_model); });

    // Drop on the empty area below the tree: back to the root.
    float rem = ImGui::GetContentRegionAvail().y;
    if (rem > 0) {
        ImGui::Dummy({ImGui::GetContentRegionAvail().x, rem});
        detail_hier::acceptEntityDrop(ctx, s_model, flecs::entity{});
    }

    // Del key deletes the selection while this panel has focus.
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        ctx.editor.selected.is_alive())
        s_model.requestDelete(ctx.editor.selected);

    // After every query above has finished: now structural changes are safe.
    s_model.apply(ctx);

    ImGui::End();
}
