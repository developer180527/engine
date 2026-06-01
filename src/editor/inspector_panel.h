#pragma once
#include "components/script_component.h"
#include <cstdio>

#include <flecs.h>
#include <imgui.h>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>
#include <utility>
#include <algorithm>

#include "engine_context.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "components/character_controller.h"
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

// Recursively find .lua files under projectRoot (skips .cache/.git/build).
// Returns {displayRelPath, projectRelativePath} (currently identical).
inline std::vector<std::pair<std::string,std::string>>
scanLuaScripts(const std::filesystem::path& projectRoot) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string,std::string>> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(projectRoot,
        fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& q = it->path();
        std::error_code dec;
        if (fs::is_directory(q, dec)) {
            std::string nm = q.filename().string();
            if (nm==".cache" || nm==".git" || nm=="build" || nm=="node_modules")
                it.disable_recursion_pending();
            continue;
        }
        if (q.extension() == ".lua") {
            std::error_code rec;
            auto rel = fs::relative(q, projectRoot, rec);
            if (!rec) { std::string r = rel.generic_string(); out.push_back({r, r}); }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
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
        ImGui::PushID((int)(uint32_t)e.id());
        if (ImGui::InputText("Name", buf, sizeof(buf)))
            n.value = buf;
        ImGui::PopID();
    }

    // ── Parent relationship ───────────────────────────────────────────
    {
        flecs::entity par = e.target(flecs::ChildOf);
        if (par && par.is_alive()) {
            ImGui::TextDisabled("Parent");
            ImGui::SameLine(90.f);
            const Name* pn = par.try_get<Name>();
            ImGui::TextColored({0.6f,0.85f,1.f,1.f},
                "%s", pn ? pn->value.c_str() : "?");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                e.remove(flecs::ChildOf, flecs::Wildcard);
                ctx.editor.sceneDirty = true;
            }
            ImGui::Spacing();
        }
    }
    // ── Transform capture for undo ────────────────────────────────────
    static Transform   s_tfBefore;
    static bool        s_tfCapturing = false;
    auto onTfActivate = [&]{
        if (!s_tfCapturing && ImGui::IsItemActivated()) {
            if (const Transform* t = e.try_get<Transform>()) s_tfBefore = *t;
            s_tfCapturing = true;
        }
    };
    auto onTfCommit = [&]{
        if (s_tfCapturing && ImGui::IsItemDeactivatedAfterEdit()) {
            if (const Transform* t = e.try_get<Transform>())
                ctx.editor.undoStack.pushTransform(e, s_tfBefore, *t);
            s_tfCapturing = false;
        }
    };
    // ── Transform ─────────────────────────────────────────────────────
    if (e.has<Transform>()) {
        detail::sectionHeader("Transform");
        Transform& t = e.get_mut<Transform>();
        ImGui::DragFloat3("Position", &t.position.x, 0.05f);
        onTfActivate(); onTfCommit();
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        bx::Vec3 eulerDeg = detail::quatToEulerDeg(t.rotation);
        if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f))
            t.rotation = detail::eulerDegToQuat(eulerDeg);
        onTfActivate(); onTfCommit();
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.editor.sceneDirty = true;
        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
        onTfActivate(); onTfCommit();
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
        if (mat) {
            detail::sectionHeader("Material");
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
            detail::texRow("Albedo",     hasAlbedo, false, mat->baseColorName);
            detail::texRow("Normal Map", hasNormal, true,  mat->normalMapName);
        } // if (mat)
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

    // ── RigidBody ──────────────────────────────────────────────────────
    if (e.has<RigidBody>()) {
        detail::sectionHeader("RigidBody");
        RigidBody& rb = e.get_mut<RigidBody>();

        // Body type
        const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
        int bti = (int)rb.bodyType;
        ImGui::Text("Body Type"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##btype", &bti, bodyTypes, 3)) {
            rb.bodyType = (PhysicsBodyType)bti; ctx.editor.sceneDirty = true;
        }

        // Shape
        const char* shapes[] = {"Box", "Sphere", "Capsule"};
        int si = (int)rb.shape;
        ImGui::Text("Shape"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##shape", &si, shapes, 3)) {
            rb.shape = (PhysicsShape)si; ctx.editor.sceneDirty = true;
        }

        // Shape-specific parameters
        if (rb.shape == PhysicsShape::Box) {
            ImGui::Text("Half Ext"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("##hext", &rb.halfExtent.x, 0.01f, 0.01f, 100.f))
                ctx.editor.sceneDirty = true;
        } else if (rb.shape == PhysicsShape::Sphere) {
            ImGui::Text("Radius"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##srad", &rb.radius, 0.01f, 0.01f, 100.f))
                ctx.editor.sceneDirty = true;
        } else {
            ImGui::Text("Radius"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##crad", &rb.radius, 0.01f, 0.01f, 100.f))
                ctx.editor.sceneDirty = true;
            ImGui::Text("Half H"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##chh", &rb.halfHeight, 0.01f, 0.01f, 100.f))
                ctx.editor.sceneDirty = true;
        }

        // Physics properties
        if (rb.bodyType == PhysicsBodyType::Dynamic) {
            ImGui::Text("Mass"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##mass", &rb.mass, 0.1f, 0.01f, 10000.f, "%.2f kg"))
                ctx.editor.sceneDirty = true;
            if (ImGui::Checkbox("Use Gravity", &rb.useGravity))
                ctx.editor.sceneDirty = true;
        }
        ImGui::Text("Restitution"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##rest", &rb.restitution, 0.f, 1.f))
            ctx.editor.sceneDirty = true;
        ImGui::Text("Friction"); ImGui::SameLine(100.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##fric", &rb.friction, 0.f, 1.f))
            ctx.editor.sceneDirty = true;

        // Remove button
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove RigidBody", {-1, 0})) {
            e.remove<RigidBody>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    } else if (!e.has<CharacterController>()) {
        // Add only when the entity has neither RigidBody nor CharacterController
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("+ Add RigidBody", {-1, 0})) {
            e.set<RigidBody>({}); ctx.editor.sceneDirty = true;
        }
    }

    // ── CharacterController ─────────────────────────────────────────────
    if (e.has<CharacterController>()) {
        detail::sectionHeader("Character Controller");
        CharacterController& cc = e.get_mut<CharacterController>();
        ImGui::Text("Radius");    ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##ccr", &cc.radius, 0.01f, 0.05f, 5.f)) ctx.editor.sceneDirty=true;
        ImGui::Text("Height");    ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##cch", &cc.height, 0.01f, 0.1f, 10.f)) ctx.editor.sceneDirty=true;
        ImGui::Text("Max Slope"); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##ccslope", &cc.maxSlopeDeg, 0.f, 80.f, "%.0f deg")) ctx.editor.sceneDirty=true;
        ImGui::Text("Step Up");   ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##ccstep", &cc.stepHeight, 0.01f, 0.f, 2.f)) ctx.editor.sceneDirty=true;
        ImGui::Text("Mass");      ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##ccmass", &cc.mass, 1.f, 1.f, 1000.f, "%.0f kg")) ctx.editor.sceneDirty=true;
        ImGui::Text("Gravity x"); ImGui::SameLine(110.f); ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##ccgrav", &cc.gravityScale, 0.05f, 0.f, 5.f)) ctx.editor.sceneDirty=true;
        ImGui::TextDisabled("Grounded: %s", cc.grounded ? "yes" : "no");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove Character Controller", {-1, 0})) {
            e.remove<CharacterController>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    } else if (!e.has<RigidBody>()) {
        ImGui::Spacing();
        if (ImGui::Button("+ Add Character Controller", {-1, 0})) {
            e.set<CharacterController>({}); ctx.editor.sceneDirty = true;
        }
    }

    // ── Script ─────────────────────────────────────────────────────────
    namespace fsi = std::filesystem;
    static std::vector<std::pair<std::string,std::string>> s_scriptList;

    // Drop target: accept an ASSET_PATH payload if it's a .lua and store it as
    // a path relative to the project root (so it always resolves at Play).
    auto acceptScriptDrop = [&](ScriptComponent* scPtr) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            fsi::path ap((const char*)pl->Data);
            if (ap.extension() == ".lua") {
                std::error_code ec;
                auto rel = fsi::relative(ap, ctx.project.projectRoot, ec);
                if (!ec) {
                    if (!scPtr) { e.set<ScriptComponent>({}); scPtr = &e.get_mut<ScriptComponent>(); }
                    scPtr->scriptPath = rel.generic_string();
                    ctx.editor.sceneDirty = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    };

    if (e.has<ScriptComponent>()) {
        detail::sectionHeader("Script");
        ScriptComponent& sc = e.get_mut<ScriptComponent>();

        ImGui::TextDisabled("Path (relative to project root)");
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", sc.scriptPath.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##scriptPath", buf, sizeof(buf))) {
            sc.scriptPath = buf; ctx.editor.sceneDirty = true;
        }
        acceptScriptDrop(&sc);

        bool valid = false;
        if (!sc.scriptPath.empty()) {
            std::error_code ec;
            valid = fsi::is_regular_file(ctx.project.projectRoot / sc.scriptPath, ec);
        }
        if (sc.scriptPath.empty())
            ImGui::TextDisabled("Drag a .lua here, or Pick");
        else if (valid)
            ImGui::TextColored({0.4f,0.9f,0.4f,1}, "* Found");
        else
            ImGui::TextColored({0.95f,0.45f,0.45f,1}, "* Not found at this path");

        if (ImGui::Button("Pick...")) {
            s_scriptList = detail::scanLuaScripts(ctx.project.projectRoot);
            ImGui::OpenPopup("##pickscript");
        }
        if (ImGui::BeginPopup("##pickscript")) {
            ImGui::TextDisabled("Lua scripts in project");
            ImGui::Separator();
            if (s_scriptList.empty()) ImGui::TextDisabled("(none found)");
            for (auto& sp : s_scriptList)
                if (ImGui::Selectable(sp.first.c_str())) {
                    sc.scriptPath = sp.second; ctx.editor.sceneDirty = true;
                    ImGui::CloseCurrentPopup();
                }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f,0.1f,0.1f,1.f));
        if (ImGui::Button("Remove Script", {-1, 0})) {
            e.remove<ScriptComponent>(); ctx.editor.sceneDirty = true;
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("+ Add Script", {-1, 0})) {
            e.set<ScriptComponent>({}); ctx.editor.sceneDirty = true;
        }
        acceptScriptDrop(nullptr);
    }
    ImGui::End();
}
