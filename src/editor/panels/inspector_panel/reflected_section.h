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
#include "editor/panels/inspector_panel/model.h"
#include "scene/reflected_serde.h"

namespace inspector_detail {

// Widgets for one struct's fields (inspect::forEachField). True if edited.
inline bool drawReflectedStruct(flecs::world& w, flecs::entity type, void* base) {
    bool edited = false;
    int  i = 0;
    int  indented = 0;
    inspect::forEachField(w, type, base, [&](const inspect::Field& f) {
        while (indented > f.depth) { ImGui::Unindent(); --indented; }
        ImGui::PushID(i++);
        ImGui::Text("%s", f.name); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        switch (f.kind) {
            case inspect::FieldKind::F32:  edited |= ImGui::DragFloat("##v", (float*)f.ptr, 0.05f); break;
            case inspect::FieldKind::F64:  edited |= ImGui::InputDouble("##v", (double*)f.ptr);     break;
            case inspect::FieldKind::Bool: edited |= ImGui::Checkbox("##v", (bool*)f.ptr);          break;
            case inspect::FieldKind::I32:  edited |= ImGui::DragInt("##v", (int32_t*)f.ptr);        break;
            case inspect::FieldKind::U32:  edited |= ImGui::DragScalar("##v", ImGuiDataType_U32, f.ptr); break;
            case inspect::FieldKind::I64:  edited |= ImGui::DragScalar("##v", ImGuiDataType_S64, f.ptr); break;
            case inspect::FieldKind::U64:  edited |= ImGui::DragScalar("##v", ImGuiDataType_U64, f.ptr); break;
            case inspect::FieldKind::Struct:
                ImGui::NewLine();                  // nested struct (vec3 etc.)
                ImGui::Indent(); ++indented;
                break;
            case inspect::FieldKind::Unsupported:
                ImGui::TextDisabled("(unsupported)");
                break;
        }
        ImGui::PopID();
    });
    while (indented-- > 0) ImGui::Unindent();
    return edited;
}

inline void drawReflectedSections(EngineContext& ctx, flecs::entity e) {
    flecs::world w = e.world();
    for (const inspect::ReflectedComponent& rc : inspect::reflectedComponents(e)) {
        ImGui::PushID((int)(uintptr_t)rc.type.id());
        sectionHeader(rc.path.c_str());
        void* ptr = inspect::componentPtr(e, rc.type);
        if (ptr && drawReflectedStruct(w, rc.type, ptr))
            inspect::markEdited(ctx, e, rc.type);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.f));
        if (ImGui::Button(("Remove " + rc.path).c_str(), {-1, 0}))
            inspect::removeComponent(ctx, e, rc.type);
        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    // Data waiting for a kit to register its type — visible, not editable.
    const auto pending = inspect::pendingComponents(e);
    if (!pending.empty()) {
        sectionHeader("Pending components");
        for (const auto& path : pending) {
            ImGui::BulletText("%s", path.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(kit not loaded — press Play once)");
        }
    }
}

} // namespace inspector_detail
