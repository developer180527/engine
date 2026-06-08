#pragma once

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "components/mesh_renderer.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "editor/inspector_panel/utils.h"

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

    ImGui::Text("Base Color");
    ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    ImGui::ColorEdit4("##baseColor", mat->baseColorFactor,
        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

    ImGui::Text("Roughness"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##rough", &mat->roughness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

    ImGui::Text("Metallic"); ImGui::SameLine(90.0f); ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##metal", &mat->metallic, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

    ImGui::Spacing(); ImGui::TextDisabled("Textures"); ImGui::Separator();
    bool hasAlbedo = mat->baseColorTexture.valid();
    bool hasNormal = mat->normalMapTexture.valid();
    texRow("Albedo",     hasAlbedo, false, mat->baseColorName);
    texRow("Normal Map", hasNormal, true,  mat->normalMapName);
}

} // namespace inspector_detail
