#pragma once

#include <imgui.h>
#include <flecs.h>

#include "editor/engine_context.h"
#include "components/rigid_body.h"
#include "components/character_controller.h"
#include "editor/panels/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawRigidBodySection(EngineContext& ctx, flecs::entity e) {
    if (e.has<RigidBody>()) {
        sectionHeader("RigidBody");
        RigidBody& rb = e.get_mut<RigidBody>();

        // Body type
        const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
        int bti = (int)rb.bodyType;
        ImGui::Text("Body Type"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        { auto bk = UndoStack::snapshotComponent(e, "rigidBody");
          if (ImGui::Combo("##btype", &bti, bodyTypes, 3)) {
              rb.bodyType = (PhysicsBodyType)bti;
              ctx.editor.undoStack.pushPropertyEdit(e, "rigidBody", bk,
                  UndoStack::snapshotComponent(e, "rigidBody"), "Change body type");
              ctx.editor.sceneDirty = true;
          }
        }

        // Shape
        const char* shapes[] = {"Box", "Sphere", "Capsule"};
        int si = (int)rb.shape;
        ImGui::Text("Shape"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        { auto bk = UndoStack::snapshotComponent(e, "rigidBody");
          if (ImGui::Combo("##shape", &si, shapes, 3)) {
              rb.shape = (PhysicsShape)si;
              ctx.editor.undoStack.pushPropertyEdit(e, "rigidBody", bk,
                  UndoStack::snapshotComponent(e, "rigidBody"), "Change physics shape");
              ctx.editor.sceneDirty = true;
          }
        }

        // Shape-specific parameters
        if (rb.shape == PhysicsShape::Box) {
            ImGui::Text("Half Ext"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat3("##hext", &rb.halfExtent.x, 0.01f, 0.01f, 100.f);
            propEdit(ctx, e, "rigidBody", "Edit RigidBody");
        } else if (rb.shape == PhysicsShape::Sphere) {
            ImGui::Text("Radius"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##srad", &rb.radius, 0.01f, 0.01f, 100.f);
            propEdit(ctx, e, "rigidBody", "Edit RigidBody");
        } else {
            ImGui::Text("Radius"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##crad", &rb.radius, 0.01f, 0.01f, 100.f);
            propEdit(ctx, e, "rigidBody", "Edit RigidBody");
            ImGui::Text("Half H"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##chh", &rb.halfHeight, 0.01f, 0.01f, 100.f);
            propEdit(ctx, e, "rigidBody", "Edit RigidBody");
        }

        // Physics properties
        if (rb.bodyType == PhysicsBodyType::Dynamic) {
            ImGui::Text("Mass"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##mass", &rb.mass, 0.1f, 0.01f, 10000.f, "%.2f kg");
            propEdit(ctx, e, "rigidBody", "Edit RigidBody");
            { auto bk = UndoStack::snapshotComponent(e, "rigidBody");
              if (ImGui::Checkbox("Use Gravity", &rb.useGravity)) {
                  ctx.editor.undoStack.pushPropertyEdit(e, "rigidBody", bk,
                      UndoStack::snapshotComponent(e, "rigidBody"), "Toggle gravity");
                  ctx.editor.sceneDirty = true;
              }
            }
        }
        ImGui::Text("Restitution"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##rest", &rb.restitution, 0.f, 1.f);
        propEdit(ctx, e, "rigidBody", "Edit RigidBody");
        ImGui::Text("Friction"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##fric", &rb.friction, 0.f, 1.f);
        propEdit(ctx, e, "rigidBody", "Edit RigidBody");

        // Remove button
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove RigidBody", {-1, 0})) {
            ctx.editor.undoStack.pushComponentRemove(e, "rigidBody", "Remove RigidBody");
            e.remove<RigidBody>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    }
    // Adding is handled by the unified "+ Add Component" menu (add_component.h).
}

} // namespace inspector_detail
