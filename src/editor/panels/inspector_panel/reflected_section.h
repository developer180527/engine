#pragma once
// ── Reflected components section ─────────────────────────────────────────────
// The generic Inspector: every meta-registered struct component on the entity
// that has no hand-written section gets an auto-generated UI straight from its
// flecs meta schema — float/int/bool members become widgets, nested structs
// (e.g. a vec3 member) recurse one level. This is what makes KIT components
// (combat::Health, ...) inspectable with zero editor code per component.
//
// Blobs still waiting for their type (kit not loaded) are listed read-only.
// v1 limitation: reflected edits don't push onto the undo stack yet (the undo
// snapshot API is keyed to the hand-written serde table).
#include <imgui.h>
#include <flecs.h>
#include <cstdint>
#include <string>
#include <vector>

#include "editor/engine_context.h"
#include "editor/panels/inspector_panel/utils.h"
#include "scene/reflected_serde.h"

namespace inspector_detail {

// Draw widgets for one struct's members at `base`. Returns true if edited.
inline bool drawReflectedStruct(flecs::world& w, flecs::entity type,
                                void* base, int depth = 0) {
    const EcsStruct* st = static_cast<const EcsStruct*>(
        ecs_get_id(w, type, ecs_id(EcsStruct)));
    if (!st) return false;

    bool edited = false;
    const ecs_member_t* members = ecs_vec_first_t(&st->members, ecs_member_t);
    const int32_t       count   = ecs_vec_count(&st->members);
    for (int32_t i = 0; i < count; ++i) {
        const ecs_member_t& m = members[i];
        void* ptr = static_cast<char*>(base) + m.offset;
        ImGui::PushID(i);
        ImGui::Text("%s", m.name); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);

        if (const EcsPrimitive* prim = static_cast<const EcsPrimitive*>(
                ecs_get_id(w, m.type, ecs_id(EcsPrimitive)))) {
            switch (prim->kind) {
                case EcsF32:  edited |= ImGui::DragFloat("##v", (float*)ptr, 0.05f);  break;
                case EcsF64:  edited |= ImGui::InputDouble("##v", (double*)ptr);      break;
                case EcsBool: edited |= ImGui::Checkbox("##v", (bool*)ptr);           break;
                case EcsI32:  edited |= ImGui::DragInt("##v", (int32_t*)ptr);         break;
                case EcsU32:  edited |= ImGui::DragScalar("##v", ImGuiDataType_U32, ptr); break;
                case EcsI64:  edited |= ImGui::DragScalar("##v", ImGuiDataType_S64, ptr); break;
                case EcsU64:  edited |= ImGui::DragScalar("##v", ImGuiDataType_U64, ptr); break;
                default:      ImGui::TextDisabled("(unsupported)");                  break;
            }
        } else if (depth < 1 && ecs_has_id(w, m.type, ecs_id(EcsStruct))) {
            ImGui::NewLine();                       // nested struct (vec3 etc.)
            ImGui::Indent();
            edited |= drawReflectedStruct(w, flecs::entity(w, m.type), ptr, depth + 1);
            ImGui::Unindent();
        } else {
            ImGui::TextDisabled("(unsupported)");
        }
        ImGui::PopID();
    }
    return edited;
}

inline void drawReflectedSections(EngineContext& ctx, flecs::entity e) {
    flecs::world w = e.world();

    // Collect first — removing a component inside each() is structural.
    std::vector<flecs::entity> comps;
    e.each([&](flecs::id id) {
        if (reflected::isReflectable(id)) comps.push_back(id.entity());
    });

    for (flecs::entity comp : comps) {
        const std::string path = reflected::componentPath(comp);
        ImGui::PushID((int)(uintptr_t)comp.id());
        sectionHeader(path.c_str());
        void* ptr = reflected::ensurePtr(w, e, comp);
        if (ptr && drawReflectedStruct(w, comp, ptr)) {
            ecs_modified_id(w, e, comp);
            ctx.editor.sceneDirty = true;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.f));
        if (ImGui::Button(("Remove " + path).c_str(), {-1, 0})) {
            ecs_remove_id(w, e, comp);
            ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // Data waiting for a kit to register its type — visible, not editable.
    if (const auto* p = e.try_get<reflected::ReflectedPending>()) {
        sectionHeader("Pending components");
        for (const auto& [path, blob] : p->blobs) {
            ImGui::BulletText("%s", path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(kit not loaded — press Play once)");
        }
    }
}

} // namespace inspector_detail
