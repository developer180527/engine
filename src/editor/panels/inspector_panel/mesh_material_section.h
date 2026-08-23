#pragma once

#include <imgui.h>
#include <flecs.h>

#include "editor/engine_context.h"
#include "components/mesh_renderer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "editor/panels/inspector_panel/utils.h"

namespace inspector_detail {

inline void drawMeshMaterialSection(EngineContext& ctx, flecs::entity e) {
    if (!e.has<MeshRenderer>()) return;

    const MeshRenderer& mr = e.get<MeshRenderer>();
    sectionHeader("Mesh Renderer");
    ImGui::TextDisabled("Handle  %u", mr.mesh.id);
    const Mesh* mesh = ctx.assets.getMesh(mr.mesh);
    if (mesh) {
        ImGui::TextDisabled("Indices  %u", mesh->indexCount);
        if (!mesh->submeshes.empty())
            ImGui::TextDisabled("Submeshes  %zu", mesh->submeshes.size());
    }

    // Resolve material (primitives may have none — skip gracefully)
    MeshRenderer& mrMut = e.get_mut<MeshRenderer>();
    MaterialHandle resolvedH = mrMut.materialOverride.valid()
        ? mrMut.materialOverride : (mesh ? mesh->material : MaterialHandle{});
    Material* mat = resolvedH.valid()
        ? const_cast<Material*>(ctx.materials.getMaterial(resolvedH)) : nullptr;
    if (!mat) return;

    sectionHeader("Material");
    bool isOverride = mrMut.materialOverride.valid();
    if (!isOverride) {
        ImGui::TextColored({1.0f, 0.65f, 0.0f, 1.0f}, "Shared");
        ImGui::SameLine();
        ImGui::TextDisabled("(edits affect all instances)");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
        if (ImGui::SmallButton("Make Unique")) {
            Material copy = *mat;
            mrMut.materialOverride = ctx.materials.addMaterial(std::move(copy));
            mat = const_cast<Material*>(
                ctx.materials.getMaterial(mrMut.materialOverride));
            ctx.editor.sceneDirty = true;
        }
    } else {
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "Override");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50.0f);
        if (ImGui::SmallButton("Revert")) { mrMut.materialOverride = {}; }
    }

    // ── Editing the BLOCKS, not a parallel set of fields ────────────────────
    // These used to be `&mat->roughness` etc. — dedicated struct members that
    // the renderer read on one code path while cooked materials used another.
    // Phase 5 step 4 removed that split, so the widgets now write into the same
    // uniform block the renderer uploads: one source of truth, and a slider
    // moves what is actually on screen rather than a shadow copy of it.
    //
    // A null means this material's shader does not DECLARE that parameter — a
    // material on somebody else's shader is not editable by a panel hard-coded
    // to the standard one. Showing it greyed is the honest rendering of that,
    // and is why each is checked rather than assumed.
    float* color = mat->baseColorFactor();
    float* rough = mat->roughness();
    float* metal = mat->metallic();

    ImGui::Text("Base Color");
    ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    if (color) {
        ImGui::ColorEdit4("##baseColor", color,
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
    } else {
        ImGui::TextDisabled("not declared by \"%s\"", mat->shaderName.c_str());
    }

    ImGui::Text("Roughness"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    if (rough) {
        ImGui::SliderFloat("##rough", rough, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
    } else {
        ImGui::TextDisabled("not declared");
    }

    ImGui::Text("Metallic"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    if (metal) {
        ImGui::SliderFloat("##metal", metal, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
    } else {
        ImGui::TextDisabled("not declared");
    }

    ImGui::Spacing(); ImGui::TextDisabled("Textures"); ImGui::Separator();
    bool hasAlbedo = mat->textureAt(0).valid();
    bool hasNormal = mat->textureAt(1).valid();
    texRow("Albedo",     hasAlbedo, false, mat->baseColorName);
    texRow("Normal Map", hasNormal, true,  mat->normalMapName);
}

} // namespace inspector_detail
