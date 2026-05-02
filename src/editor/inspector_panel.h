#pragma once

#include <flecs.h>
#include <imgui.h>
#include <bx/math.h>
#include <cmath>

#include "editor_state.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"

// Inspector panel.
//
// Shows the components of the currently selected entity and lets the user
// edit them. Reads selection from EditorState; reads/writes components
// directly on the selected entity.
//
// The inspector is "deeply opinionated" about the engine's component types —
// each component type needs its own editing UI. As the engine grows, this
// function will get longer (one if-block per component). When that becomes
// unwieldy, we'll factor it into per-component "inspector functions" that
// register themselves at startup. For now, conditionals are simpler than
// indirection.
//
// Rotation is stored as quaternion but displayed as Euler angles in degrees.
// We do the conversion at the UI boundary: read quat -> show as Euler, edit
// Euler -> write back as quat. Each frame, the displayed Euler values are
// re-derived from the current quaternion (so rotations from the Spinner
// system or other sources stay in sync with the inspector).

namespace detail {

// Convert a quaternion to Euler angles (in degrees) for human-readable UI.
// Uses the standard XYZ extraction: pitch around X, yaw around Y, roll around Z.
// Has gimbal-lock degenerate cases at pitch = +-90 deg, but those are rare
// and the inspector behavior in those cases is "weird but recoverable" —
// the user just nudges another field and things resolve.
inline bx::Vec3 quatToEulerDeg(const bx::Quaternion& q) {
    // Pitch (X)
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    float pitch;
    if      (sinp >=  1.0f) pitch =  bx::kPiHalf;
    else if (sinp <= -1.0f) pitch = -bx::kPiHalf;
    else                    pitch = std::asin(sinp);

    // Yaw (Y)
    const float sinyCosp =  2.0f * (q.w * q.y + q.x * q.z);
    const float cosyCosp =  1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float yaw      = std::atan2(sinyCosp, cosyCosp);

    // Roll (Z)
    const float sinrCosp =  2.0f * (q.w * q.z + q.x * q.y);
    const float cosrCosp =  1.0f - 2.0f * (q.z * q.z + q.x * q.x);
    const float roll     = std::atan2(sinrCosp, cosrCosp);

    constexpr float kRadToDeg = 57.2957795f;  // 180 / pi
    return { pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg };
}

inline bx::Quaternion eulerDegToQuat(const bx::Vec3& eulerDeg) {
    constexpr float kDegToRad = 0.01745329f;  // pi / 180
    const float pitch = eulerDeg.x * kDegToRad;
    const float yaw   = eulerDeg.y * kDegToRad;
    const float roll  = eulerDeg.z * kDegToRad;

    // Build axis-angle quats then compose: yaw * pitch * roll
    // (matches the extraction order above).
    const bx::Quaternion qPitch = bx::fromAxisAngle({1, 0, 0}, pitch);
    const bx::Quaternion qYaw   = bx::fromAxisAngle({0, 1, 0}, yaw);
    const bx::Quaternion qRoll  = bx::fromAxisAngle({0, 0, 1}, roll);

    return bx::normalize(bx::mul(qYaw, bx::mul(qPitch, qRoll)));
}

} // namespace detail

inline void drawInspectorPanel(flecs::world&, EditorState& editor) {
    ImGui::Begin("Inspector");

    if (!editor.selected.is_alive()) {
        ImGui::TextDisabled("(no entity selected)");
        ImGui::End();
        return;
    }

    flecs::entity e = editor.selected;

    // ---- Name ----
    if (e.has<Name>()) {
        Name& n = e.get_mut<Name>();
        // ImGui InputText needs a fixed buffer; copy in, edit, copy back.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", n.value.c_str());

        if (ImGui::InputText("Name", buf, sizeof(buf))) {
            n.value = buf;
        }
    }

    ImGui::Separator();

    // ---- Transform ----
    if (e.has<Transform>()) {
        Transform& t = e.get_mut<Transform>();
        ImGui::Text("Transform");

        ImGui::DragFloat3("Position", &t.position.x, 0.05f);

        // Rotation: convert to Euler for display, back to quat on edit.
        bx::Vec3 eulerDeg = detail::quatToEulerDeg(t.rotation);
        if (ImGui::DragFloat3("Rotation", &eulerDeg.x, 0.5f)) {
            t.rotation = detail::eulerDegToQuat(eulerDeg);
        }

        ImGui::DragFloat3("Scale", &t.scale.x, 0.05f, 0.01f, 100.0f);
    }

    ImGui::Separator();

    // ---- Spinner ----
    if (e.has<Spinner>()) {
        Spinner& s = e.get_mut<Spinner>();
        ImGui::Text("Spinner");
        ImGui::DragFloat("Yaw speed",   &s.speedYaw,   0.05f);
        ImGui::DragFloat("Pitch speed", &s.speedPitch, 0.05f);
    }

    ImGui::Separator();

    // ---- MeshRenderer (read-only display for now) ----
    if (e.has<MeshRenderer>()) {
        const MeshRenderer& mr = e.get<MeshRenderer>();
        ImGui::Text("MeshRenderer");
        ImGui::Text("  Mesh handle: %u", mr.mesh.id);
    }

    ImGui::End();
}
