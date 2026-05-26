#pragma once

#include <flecs.h>
#include <imgui.h>
#include <bx/math.h>
#include <cmath>
#include <cstring>

#include "engine_context.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "components/camera.h"

namespace detail {

inline bx::Vec3 quatToEulerDeg(const bx::Quaternion& q) {
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    float pitch;
    if      (sinp >=  1.0f) pitch =  bx::kPiHalf;
    else if (sinp <= -1.0f) pitch = -bx::kPiHalf;
    else                    pitch = std::asin(sinp);
    const float sinyCosp = 2.0f * (q.w * q.y + q.x * q.z);
    const float cosyCosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float yaw      = std::atan2(sinyCosp, cosyCosp);
    const float sinrCosp = 2.0f * (q.w * q.z + q.x * q.y);
    const float cosrCosp = 1.0f - 2.0f * (q.z * q.z + q.x * q.x);
    const float roll     = std::atan2(sinrCosp, cosrCosp);
    constexpr float kRadToDeg = 57.2957795f;
    return { pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg };
}

inline bx::Quaternion eulerDegToQuat(const bx::Vec3& eulerDeg) {
    constexpr float kDegToRad = 0.01745329f;
    const bx::Quaternion qPitch = bx::fromAxisAngle({1,0,0}, eulerDeg.x * kDegToRad);
    const bx::Quaternion qYaw   = bx::fromAxisAngle({0,1,0}, eulerDeg.y * kDegToRad);
    const bx::Quaternion qRoll  = bx::fromAxisAngle({0,0,1}, eulerDeg.z * kDegToRad);
    return bx::normalize(bx::mul(qYaw, bx::mul(qPitch, qRoll)));
}

// Small colored circle — used as texture slot status indicator
inline void texDot(bool loaded, bool isNormal = false) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 5.0f;
    ImVec2 center = { p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f };
    ImU32 col;
    if (!loaded)      col = IM_COL32(90, 90, 90, 255);      // gray = no texture
    else if (isNormal) col = IM_COL32(100, 160, 255, 255);  // blue = normal map
    else               col = IM_COL32(80, 200, 120, 255);   // green = albedo
    ImGui::GetWindowDrawList()->AddCircleFilled(center, r, col, 12);
    ImGui::Dummy({r * 2.0f + 4.0f, ImGui::GetTextLineHeight()});
}

// Section header with subtle background bar
inline void sectionHeader(const char* label) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w  = ImGui::GetContentRegionAvail().x;
    float h  = ImGui::GetTextLineHeight() + 4.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, {p.x + w, p.y + h},
        IM_COL32(60, 60, 70, 180), 3.0f);
    ImGui::SetCursorScreenPos({p.x + 6.0f, p.y + 2.0f});
    ImGui::TextUnformatted(label);
    ImGui::SetCursorScreenPos({p.x, p.y + h + 2.0f});
}

// Texture row: dot + label + filename (truncated)
inline void texRow(const char* slot, bool loaded, bool isNormal,
                   const std::string& name) {
    texDot(loaded, isNormal);
    ImGui::SameLine(0, 6);
    if (loaded) {
        ImGui::TextUnformatted(slot);
        if (!name.empty()) {
            ImGui::SameLine();
            // Truncate long filenames
            const char* fn = name.c_str();
            if (name.size() > 28) {
                ImGui::TextDisabled("...%s", fn + name.size() - 25);
            } else {
                ImGui::TextDisabled("%s", fn);
            }
        }
    } else {
        ImGui::TextDisabled("%s  —  none", slot);
    }
}

} // namespace detail

inline void drawInspectorPanel(EngineContext& ctx) {
    ImGui::Begin("Inspector");

    if (!ctx.editor.selected.is_alive()) {
        ImGui::TextDisabled("(no entity selected)");
        ImGui::End();
        return;
    }

    flecs::entity e = ctx.editor.selected;

    // ── Name ──────────────────────────────────────────────────────────
    if (e.has<Name>()) {
        Name& n = e.get_mut<Name>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", n.value.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf)))
            n.value = buf;
    }

    // ── Transform ─────────────────────────────────────────────────────
    if (e.has<Transform>()) {
        detail::sectionHeader("Transform");
        Transform& t = e.get_mut<Transform>();
        ImGui::DragFloat3("Position", &t.position.x, 0.05f);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        bx::Vec3 eulerDeg = detail::quatToEulerDeg(t.rotation);
        if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f))
            t.rotation = detail::eulerDegToQuat(eulerDeg);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
    }

    // ── Spinner ───────────────────────────────────────────────────────
    if (e.has<Spinner>()) {
        detail::sectionHeader("Spinner");
        Spinner& s = e.get_mut<Spinner>();
        ImGui::DragFloat("Yaw speed",   &s.speedYaw,   0.05f);
        ImGui::DragFloat("Pitch speed", &s.speedPitch, 0.05f);
    }

    // ── MeshRenderer + Material ───────────────────────────────────────
    if (e.has<MeshRenderer>()) {
        const MeshRenderer& mr = e.get<MeshRenderer>();
        detail::sectionHeader("Mesh Renderer");
        ImGui::TextDisabled("Handle  %u", mr.mesh.id);

        const Mesh* mesh = ctx.assets.getMesh(mr.mesh);
        if (!mesh) { ImGui::End(); return; }

        // Mesh stats
        ImGui::TextDisabled("Indices  %u", mesh->indexCount);
        if (!mesh->submeshes.empty())
            ImGui::TextDisabled("Submeshes  %zu", mesh->submeshes.size());

        // Resolve material — per-entity override wins over shared mesh material
        MeshRenderer& mrMut = e.get_mut<MeshRenderer>();
        MaterialHandle resolvedH = mrMut.materialOverride.valid()
            ? mrMut.materialOverride : mesh->material;
        if (!resolvedH.valid()) { ImGui::End(); return; }
        Material* mat = const_cast<Material*>(
            ctx.materials.getMaterial(resolvedH));
        if (!mat) { ImGui::End(); return; }

        detail::sectionHeader("Material");

        // Shared vs override badge
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
            if (ImGui::SmallButton("Revert")) {
                mrMut.materialOverride = {};
                ImGui::End(); return;
            }
        }

        // ── Base Color ────────────────────────────────────────────────
        ImGui::Text("Base Color");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.0f + 90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit4("##baseColor", mat->baseColorFactor,
                          ImGuiColorEditFlags_Float |
                          ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

        // ── PBR sliders ───────────────────────────────────────────────
        ImGui::Text("Roughness");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##rough", &mat->roughness, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

        ImGui::Text("Metallic");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##metal", &mat->metallic,  0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

        // ── Texture slots ─────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::TextDisabled("Textures");
        ImGui::Separator();

        bool hasAlbedo  = mat->baseColorTexture.valid();
        bool hasNormal  = mat->normalMapTexture.valid();

        detail::texRow("Albedo",     hasAlbedo, false, mat->baseColorName);
        detail::texRow("Normal Map", hasNormal, true,  mat->normalMapName);
    }

    // ── Camera ────────────────────────────────────────────────────
    if (e.has<Camera>()) {
        detail::sectionHeader("Camera");
        Camera& cam = e.get_mut<Camera>();

        // Primary
        ImGui::Text("Primary");
        ImGui::SameLine(90.0f);
        if (ImGui::Checkbox("##primary", &cam.isPrimary))
            ctx.editor.sceneDirty = true;
        if (cam.isPrimary) {
            ImGui::SameLine();
            ImGui::TextColored({0.3f,0.9f,0.3f,1}, "Game Camera");
        }

        // Projection
        ImGui::Text("Projection");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        const char* projNames[] = {"Perspective", "Orthographic"};
        int projIdx = (int)cam.projection;
        if (ImGui::Combo("##proj", &projIdx, projNames, 2)) {
            cam.projection = (ProjectionType)projIdx;
            ctx.editor.sceneDirty = true;
        }

        if (cam.projection == ProjectionType::Perspective) {
            ImGui::Text("FOV");
            ImGui::SameLine(90.0f);
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##fov", &cam.fov, 10.0f, 170.0f, "%.1f deg");
            if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        } else {
            ImGui::Text("Size");
            ImGui::SameLine(90.0f);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##orthoSize", &cam.orthoSize, 0.1f, 0.1f, 1000.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        }

        ImGui::Text("Near");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##near", &cam.nearPlane, 0.001f, 0.001f, 100.0f, "%.3f");
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

        ImGui::Text("Far");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##far", &cam.farPlane, 1.0f, 1.0f, 100000.0f);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;

        ImGui::Spacing();
        ImGui::Text("Clear");
        ImGui::SameLine(90.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit4("##clear", cam.clearColor,
            ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
    }

    ImGui::End();
}
