#pragma once

#include <cstdio>
#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/name.h"
#include "core/transform.h"
#include "core/entity_id_util.h"
#include "editor/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawNameSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<Name>()) return;
    Name& n = e.get_mut<Name>();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", n.value.c_str());
    ImGui::PushID((int)(uint32_t)e.id());
    if (ImGui::InputText("Name", buf, sizeof(buf)))
        n.value = buf;
    propEdit(ctx, e, "name", "Rename");
    ImGui::PopID();
}

inline void drawParentSection(EngineContext& ctx, flecs::entity e) {
    flecs::entity par = e.target(flecs::ChildOf);
    if (!par || !par.is_alive()) return;

    ImGui::TextDisabled("Parent");
    ImGui::SameLine(90.f);
    const Name* pn = par.try_get<Name>();
    ImGui::TextColored({0.6f,0.85f,1.f,1.f},
        "%s", pn ? pn->value.c_str() : "?");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        flecs::entity op = e.target(flecs::ChildOf);
        uint64_t opid = (op && op.is_alive()) ? ensureEntityId(op) : 0;
        Transform otf{}; if (const Transform* pt = e.try_get<Transform>()) otf = *pt;
        e.remove(flecs::ChildOf, flecs::Wildcard);
        Transform ntf{}; if (const Transform* pt = e.try_get<Transform>()) ntf = *pt;
        ctx.editor.undoStack.pushReparent(e, opid, otf, 0, ntf);
        ctx.editor.sceneDirty = true;
    }
    ImGui::Spacing();
}

} // namespace inspector_detail
