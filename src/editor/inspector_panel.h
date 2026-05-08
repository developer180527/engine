#pragma once

#include <flecs.h>
#include <imgui.h>
#include <bx/math.h>
#include <cmath>

#include "engine_context.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"

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
    const bx::Quaternion qPitch =
        bx::fromAxisAngle({1,0,0}, eulerDeg.x * kDegToRad);
    const bx::Quaternion qYaw   =
        bx::fromAxisAngle({0,1,0}, eulerDeg.y * kDegToRad);
    const bx::Quaternion qRoll  =
        bx::fromAxisAngle({0,0,1}, eulerDeg.z * kDegToRad);
    return bx::normalize(bx::mul(qYaw, bx::mul(qPitch, qRoll)));
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

    if (e.has<Name>()) {
        Name& n = e.get_mut<Name>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", n.value.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf)))
            n.value = buf;
    }

    ImGui::Separator();

    if (e.has<Transform>()) {
        Transform& t = e.get_mut<Transform>();
        ImGui::Text("Transform");
        ImGui::DragFloat3("Position", &t.position.x, 0.05f);
        bx::Vec3 eulerDeg = detail::quatToEulerDeg(t.rotation);
        if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f))
            t.rotation = detail::eulerDegToQuat(eulerDeg);
        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
    }

    ImGui::Separator();

    if (e.has<Spinner>()) {
        Spinner& s = e.get_mut<Spinner>();
        ImGui::Text("Spinner");
        ImGui::DragFloat("Yaw speed",   &s.speedYaw,   0.05f);
        ImGui::DragFloat("Pitch speed", &s.speedPitch, 0.05f);
    }

    ImGui::Separator();

    if (e.has<MeshRenderer>()) {
        const MeshRenderer& mr = e.get<MeshRenderer>();
        ImGui::Text("MeshRenderer");
        ImGui::Text("  Mesh handle: %u", mr.mesh.id);
    }

    ImGui::End();
}
