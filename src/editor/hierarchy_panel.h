#pragma once
#include <imgui.h>
#include <flecs.h>
#include <string>
#include "engine_context.h"
#include "components/name.h"
#include "components/camera.h"
#include "components/spinner.h"
#include "components/rigid_body.h"
#include "render/primitive_library.h"
#include "core/transform.h"
#include "editor/asset_browser/spawn.h"
#include "core/transform_utils.h"

// ── ReparentOp ─────────────────────────────────────────────────────────────
// Structural ECS changes (add/remove ChildOf) cannot happen inside a flecs
// .each() callback — the table is locked during iteration (LOCKED_STORAGE).
// We collect the desired reparent into this struct and apply it AFTER the
// query completes, exactly like toDelete.
struct ReparentOp {
    flecs::entity child;
    flecs::entity newParent; // invalid entity = unparent to root
    bool pending = false;
};

namespace detail_hier {

// ── Add-entity menu items (call from any popup context) ───────────────────
inline void drawAddMenuItems(EngineContext& ctx) {
    ImGui::TextDisabled("Add Entity");
    ImGui::Separator();

    if (ImGui::MenuItem("[cam] Camera")) {
        std::string name = uniqueEntityName(ctx.ecs, "Camera");
        Transform t{}; t.scale={1,1,1}; t.rotation={0,0,0,1};
        Camera cam; cam.isPrimary = false;
        ctx.ecs.entity(name.c_str())
            .set<Transform>(t).set<Name>({name}).set<Camera>(cam);
        ctx.editor.sceneDirty = true;
    }

    ImGui::Separator();
    ImGui::TextDisabled("Primitives");
    if (ctx.primitives && ctx.primitives->ready()) {
        auto spawnPrim = [&](const char* label, MeshHandle h) {
            if (!h.valid()) return;
            std::string name = uniqueEntityName(ctx.ecs, label);
            Transform t{}; t.scale={1,1,1}; t.rotation={0,0,0,1};
            auto e = ctx.ecs.entity(name.c_str())
                .set<Transform>(t).set<Name>({name})
                .set<MeshRenderer>({h});
            ctx.editor.selected = e;
            ctx.editor.sceneDirty = true;
        };
        if (ImGui::MenuItem("  Cube"))   spawnPrim("Cube",   ctx.primitives->cube());
        if (ImGui::MenuItem("  Sphere")) spawnPrim("Sphere", ctx.primitives->sphere());
        if (ImGui::MenuItem("  Plane"))  spawnPrim("Plane",  ctx.primitives->plane());
    } else {
        ImGui::BeginDisabled();
        ImGui::MenuItem("  Cube");
        ImGui::MenuItem("  Sphere");
        ImGui::MenuItem("  Plane");
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled();
    ImGui::MenuItem("  Torus");
    ImGui::MenuItem("  Capsule");
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Physics");
    if (ctx.editor.selected.is_alive() &&
        ImGui::MenuItem("RigidBody (add to selected)")) {
        if (!ctx.editor.selected.has<RigidBody>()) {
            ctx.editor.selected.set<RigidBody>({});
            ctx.editor.sceneDirty = true;
        }
    }

    ImGui::Separator();
    ImGui::BeginDisabled();
    ImGui::MenuItem("  Empty Object");
    ImGui::MenuItem("  Point Light");
    ImGui::MenuItem("  Directional Light");
    ImGui::EndDisabled();
}

// ── Add-entity button popup wrapper ──────────────────────────────────────
inline void drawAddPopup(EngineContext& ctx) {
    if (!ImGui::BeginPopup("##addEntity")) return;
    drawAddMenuItems(ctx);
    ImGui::EndPopup();
}

// ── Recursive entity tree node ─────────────────────────────────────────────
// toDelete and reparentOp are collected here but applied AFTER all queries
// complete, avoiding the LOCKED_STORAGE crash on structural changes.
inline void drawEntityNode(flecs::entity e, EngineContext& ctx,
                            flecs::entity& toDelete,
                            ReparentOp&   reparentOp) {
    const Name* n = e.try_get<Name>();
    if (!n) return;

    char label[128];
    if (e.has<Camera>())
        snprintf(label, sizeof(label), "[Cam] %s", n->value.c_str());
    else
        snprintf(label, sizeof(label), "%s", n->value.c_str());

    // Count named children
    int childCount = 0;
    e.children([&](flecs::entity c) { if (c.has<Name>()) ++childCount; });

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (childCount == 0)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (ctx.editor.selected == e)
        flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)e.id(), flags, "%s", label);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        ctx.editor.selected = e;

    // Drag source
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        flecs::entity_t id = e.id();
        ImGui::SetDragDropPayload("ENTITY_ID", &id, sizeof(id));
        ImGui::Text("  %s", n->value.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop target — reparent dropped entity under this entity (deferred)
    if (ImGui::BeginDragDropTarget()) {
        if (auto* pl = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
            flecs::entity_t dragId = *(flecs::entity_t*)pl->Data;
            flecs::entity dragged  = ctx.ecs.entity(dragId);
            if (dragged && dragged.is_alive() && dragged != e)
                reparentOp = {dragged, e, true};
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem("##entityCtx")) {
        if (ImGui::MenuItem("Select")) ctx.editor.selected = e;
        flecs::entity par = e.target(flecs::ChildOf);
        if (par && par.is_alive())
            if (ImGui::MenuItem("Unparent"))
                reparentOp = {e, flecs::entity{}, true}; // deferred
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, {1,0.3f,0.3f,1});
        if (ImGui::MenuItem("Delete")) toDelete = e;
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    // Recurse children
    if (open && childCount > 0) {
        e.children([&](flecs::entity child) {
            if (child.has<Name>())
                drawEntityNode(child, ctx, toDelete, reparentOp);
        });
        ImGui::TreePop();
    }
}

} // namespace detail_hier

// ── drawHierarchyPanel ─────────────────────────────────────────────────────
inline void drawHierarchyPanel(EngineContext& ctx) {
    ImGui::Begin("Hierarchy");

    // Header
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##addEntity");
    ImGui::SameLine();
    int total = 0;
    ctx.ecs.query_builder<const Name>().without<Spinner>()
        .build().each([&](flecs::entity, const Name&) { ++total; });
    ImGui::TextDisabled("%d entities", total);

    detail_hier::drawAddPopup(ctx);

    if (ImGui::BeginPopupContextWindow("##hierCtx",
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        detail_hier::drawAddMenuItems(ctx); // items directly — no nested BeginPopup
        ImGui::EndPopup();
    }

    ImGui::Separator();

    flecs::entity toDelete{};
    ReparentOp    reparentOp{};

    // Only show root entities (no named parent)
    ctx.ecs.query_builder<const Name>()
        .without<Spinner>()
        .build()
        .each([&](flecs::entity e, const Name&) {
            flecs::entity par = e.target(flecs::ChildOf);
            bool isRoot = !par || !par.is_alive() || !par.has<Name>();
            if (isRoot)
                detail_hier::drawEntityNode(e, ctx, toDelete, reparentOp);
        });

    // Drop on empty area → unparent to root (deferred)
    float rem = ImGui::GetContentRegionAvail().y;
    if (rem > 0) {
        ImGui::Dummy({ImGui::GetContentRegionAvail().x, rem});
        if (ImGui::BeginDragDropTarget()) {
            if (auto* pl = ImGui::AcceptDragDropPayload("ENTITY_ID")) {
                flecs::entity_t id = *(flecs::entity_t*)pl->Data;
                flecs::entity dropped = ctx.ecs.entity(id);
                if (dropped && dropped.is_alive())
                    reparentOp = {dropped, flecs::entity{}, true};
            }
            ImGui::EndDragDropTarget();
        }
    }

    // ── Apply deferred reparent — preserves world transform ───────────────
    if (reparentOp.pending && reparentOp.child && reparentOp.child.is_alive()) {
        // Snapshot child world transform BEFORE structural change
        float childWorld[16];
        getWorldMatrix(reparentOp.child, childWorld);
        bx::Vec3       wPos{0,0,0}; bx::Quaternion wRot{0,0,0,1}; bx::Vec3 wScale{1,1,1};
        decomposeMatrix(childWorld, wPos, wRot, wScale);

        // Structural changes (outside any query — safe here)
        reparentOp.child.remove(flecs::ChildOf, flecs::Wildcard);
        if (reparentOp.newParent && reparentOp.newParent.is_alive()) {
            reparentOp.child.add(flecs::ChildOf, reparentOp.newParent);
            // Compute child local = child_world * inverse(parent_world)
            float parentWorld[16], parentInv[16];
            getWorldMatrix(reparentOp.newParent, parentWorld);
            bx::mtxInverse(parentInv, parentWorld);
            bx::Vec3 lPos{0,0,0}; bx::Quaternion lRot{0,0,0,1}; bx::Vec3 lScale{1,1,1};
            float localMtx[16];
            bx::mtxMul(localMtx, childWorld, parentInv);
            decomposeMatrix(localMtx, lPos, lRot, lScale);
            Transform& t = reparentOp.child.get_mut<Transform>();
            t.position = lPos; t.rotation = lRot; t.scale = lScale;
        } else {
            // Unparented to root — local becomes world
            Transform& t = reparentOp.child.get_mut<Transform>();
            t.position = wPos; t.rotation = wRot; t.scale = wScale;
        }
        ctx.editor.sceneDirty = true;
    }

    // ── Apply deferred delete ──────────────────────────────────────────────
    if (toDelete && toDelete.is_alive()) {
        if (ctx.editor.selected == toDelete) ctx.editor.selected = {};
        toDelete.destruct();
        ctx.editor.sceneDirty = true;
    }

    // Del key
    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        ctx.editor.selected.is_alive()) {
        ctx.editor.selected.destruct();
        ctx.editor.selected = {};
        ctx.editor.sceneDirty = true;
    }

    ImGui::End();
}
