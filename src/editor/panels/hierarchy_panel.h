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
#include "core/transform_utils.h"
#include "core/entity_id_util.h"

// ── ReparentOp ─────────────────────────────────────────────────────────────
// Structural ECS changes (add/remove ChildOf) cannot happen inside a flecs
// .each() callback — the table is locked during iteration (LOCKED_STORAGE).
// We collect the desired reparent into this struct and apply it AFTER the
// query completes, exactly like toDelete.
struct ReparentOp {
    flecs::entity child;
    flecs::entity newParent;
    bool pending = false;
    flecs::entity oldParent;            // for undo (invalid = was root)
    Transform     oldLocalTransform{}; // for undo
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
        { auto ne = ctx.ecs.entity(name.c_str())
              .set<Transform>(t).set<Name>({name}).set<Camera>(cam);
          ctx.editor.undoStack.pushEntityAdd(ne); }
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
            ctx.editor.undoStack.pushEntityAdd(e);
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
    ImGui::TextDisabled("Lights");
    {
        auto spawnLight = [&](const char* label, const Transform& t, const Light& lc) {
            std::string nm = uniqueEntityName(ctx.ecs, label);
            auto e = ctx.ecs.entity(nm.c_str())
                .set<Transform>(t).set<Name>({nm}).set<Light>(lc);
            ctx.editor.undoStack.pushEntityAdd(e);
            ctx.editor.selected = e;
            ctx.editor.sceneDirty = true;
        };
        if (ImGui::MenuItem("[sun] Directional Light")) {
            Transform t{}; t.scale={1,1,1};
            t.rotation={-0.70710678f, 0.0f, 0.0f, 0.70710678f};
            Light lc; lc.type = LightType::Directional; lc.intensity = 3.0f;
            spawnLight("DirectionalLight", t, lc);
        }
        if (ImGui::MenuItem("[pt] Point Light")) {
            Transform t{}; t.scale={1,1,1}; t.rotation={0,0,0,1};
            t.position={0.0f, 5.0f, 0.0f};
            Light lc; lc.type = LightType::Point; lc.range = 20.0f; lc.intensity = 25.0f;
            spawnLight("PointLight", t, lc);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("  Empty Object")) {
        std::string nm = uniqueEntityName(ctx.ecs, "Empty");
        Transform t{}; t.scale={1,1,1}; t.rotation={0,0,0,1};
        auto e = ctx.ecs.entity(nm.c_str()).set<Transform>(t).set<Name>({nm});
        ctx.editor.undoStack.pushEntityAdd(e);
        ctx.editor.selected = e;
        ctx.editor.sceneDirty = true;
    }
}

// ── Add-entity button popup wrapper ──────────────────────────────────────
inline void drawAddPopup(EngineContext& ctx) {
    if (!ImGui::BeginPopup("##addEntity")) return;
    drawAddMenuItems(ctx);
    ImGui::EndPopup();
}

// TODO: Cache hierarchy nodes/entities in a HierarchyModel and render from
// the cached tree. Avoid recursive ECS traversal every frame for large scenes.
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

    ImGui::PushID((int)(uint32_t)e.id());
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", label);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        ctx.editor.selected = e;

    // Drag source
    if (ImGui::BeginDragDropSource()) {
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
            if (dragged && dragged.is_alive() && dragged != e && !isAncestorOf(dragged, e)) {
                flecs::entity dp = dragged.target(flecs::ChildOf);
                Transform oldTf{};
                if (const Transform* dt=dragged.try_get<Transform>()) oldTf=*dt;
                reparentOp = {dragged, e, true, dp, oldTf};
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Right-click context menu
    if (ImGui::BeginPopupContextItem("##ctx")) {
        if (ImGui::MenuItem("Select")) ctx.editor.selected = e;
        flecs::entity par = e.target(flecs::ChildOf);
        if (par && par.is_alive())
            if (ImGui::MenuItem("Unparent")) {
                flecs::entity ep = e.target(flecs::ChildOf);
                Transform oldTf{};
                if (const Transform* et=e.try_get<Transform>()) oldTf=*et;
                reparentOp = {e, flecs::entity{}, true, ep, oldTf};
            }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, {1,0.3f,0.3f,1});
        if (ImGui::MenuItem("Delete")) toDelete = e;
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    if (open && childCount > 0) {
        e.children([&](flecs::entity child) {
            if (child.has<Name>())
                drawEntityNode(child, ctx, toDelete, reparentOp);
        });
        ImGui::TreePop();
    }
    ImGui::PopID(); // always last — after TreePop for open nodes, safe for leaves
}

} // namespace detail_hier

// ── drawHierarchyPanel ─────────────────────────────────────────────────────
inline void drawHierarchyPanel(EngineContext& ctx) {
    ImGui::Begin(ICON_FA_SITEMAP " Hierarchy");

    // Header
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##addEntity");
    ImGui::SameLine();
    // TODO: Cache frequently used flecs queries instead of rebuilding them every frame.
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

    // TODO: Add hierarchy search/filter bar.
    // TODO: Introduce SelectionManager to support scene, asset, runtime and multi-selection.

    flecs::entity toDelete{};
    ReparentOp    reparentOp{};

    // TODO: Use a cached query and HierarchyModel for hierarchy rendering.
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
                if (dropped && dropped.is_alive()) {
                    flecs::entity dp2 = dropped.target(flecs::ChildOf);
                    Transform oldTf2{};
                    if (const Transform* dt2=dropped.try_get<Transform>()) oldTf2=*dt2;
                    reparentOp = {dropped, flecs::entity{}, true, dp2, oldTf2};
                }
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
            safeInvert(parentInv, parentWorld);
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
        // Push undo command with old/new state (ids resolved here — post-query, safe)
        uint64_t oldPid = (reparentOp.oldParent && reparentOp.oldParent.is_alive())
                            ? ensureEntityId(reparentOp.oldParent) : 0;
        uint64_t newPid = (reparentOp.newParent && reparentOp.newParent.is_alive())
                            ? ensureEntityId(reparentOp.newParent) : 0;
        Transform newTf{};
        if (const Transform* nt = reparentOp.child.try_get<Transform>()) newTf = *nt;
        ctx.editor.undoStack.pushReparent(reparentOp.child, oldPid,
            reparentOp.oldLocalTransform, newPid, newTf);
        ctx.editor.sceneDirty = true;
    }

    // ── Apply deferred delete ──────────────────────────────────────────────
    if (toDelete && toDelete.is_alive()) {
        ctx.editor.undoStack.pushEntityDelete(toDelete);
        if (ctx.editor.selected == toDelete) ctx.editor.selected = {};
        toDelete.destruct();
        ctx.editor.sceneDirty = true;
    }

    // Del key
    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        ctx.editor.selected.is_alive()) {
        ctx.editor.undoStack.pushEntityDelete(ctx.editor.selected);
        ctx.editor.selected.destruct();
        ctx.editor.selected = {};
        ctx.editor.sceneDirty = true;
    }

    ImGui::End();
}
