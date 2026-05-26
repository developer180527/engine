#pragma once
#include <imgui.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include "engine_context.h"
#include "components/camera.h"
#include "core/transform.h"

namespace detail_gv {

// Rotate a vector by a quaternion (q * v * q^-1)
inline bx::Vec3 quatRotate(const bx::Quaternion& q, const bx::Vec3& v) {
    const bx::Vec3 qv  = {q.x, q.y, q.z};
    const bx::Vec3 uv  = bx::cross(qv, v);
    const bx::Vec3 uuv = bx::cross(qv, uv);
    return bx::add(v, bx::add(bx::mul(uv, 2.0f * q.w), bx::mul(uuv, 2.0f)));
}

} // namespace detail_gv

// Returns true + fills view/proj if a primary Camera entity is found.
inline bool findPrimaryCamera(EngineContext& ctx,
                              float view[16], float proj[16],
                              float aspect,
                              float clearColor[4]) {
    bool found = false;
    ctx.ecs.query_builder<const Transform, const Camera>()
        .build()
        .each([&](flecs::entity, const Transform& t, const Camera& c) {
            if (!c.isPrimary || found) return;
            found = true;

            // View matrix from Transform
            bx::Quaternion q{t.rotation.x, t.rotation.y,
                             t.rotation.z, t.rotation.w};
            bx::Vec3 pos = {t.position.x, t.position.y, t.position.z};
            bx::Vec3 fwd = detail_gv::quatRotate(q, {0.0f, 0.0f, -1.0f});
            bx::Vec3 up  = detail_gv::quatRotate(q, {0.0f, 1.0f,  0.0f});
            bx::Vec3 at  = bx::add(pos, fwd);
            bx::mtxLookAt(view, pos, at, up);

            // Projection matrix
            const bool rhNdc = bgfx::getCaps()->homogeneousDepth;
            if (c.projection == ProjectionType::Perspective) {
                bx::mtxProj(proj, c.fov, aspect,
                            c.nearPlane, c.farPlane, rhNdc);
            } else {
                const float h = c.orthoSize;
                const float w = h * aspect;
                bx::mtxOrtho(proj, -w, w, -h, h,
                             c.nearPlane, c.farPlane, 0.0f, rhNdc);
            }
            std::memcpy(clearColor, c.clearColor, 16);
        });
    return found;
}

inline void drawGameViewPanel(bgfx::TextureHandle gameTex,
                              bool hasCam, int sceneW, int sceneH) {
    ImGui::Begin("Game View");

    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (!hasCam) {
        // Centered "no camera" message
        const char* msg = "No Camera in scene";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos({(avail.x - ts.x) * 0.5f + ImGui::GetStyle().WindowPadding.x,
                             (avail.y - ts.y) * 0.5f + ImGui::GetStyle().WindowPadding.y});
        ImGui::TextDisabled("%s", msg);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        const char* hint = "Add one via  Hierarchy > + Add > Camera";
        ts = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPosX((avail.x - ts.x) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("%s", hint);
    } else if (!bgfx::isValid(gameTex)) {
        ImGui::TextDisabled("Initialising...");
    } else {
        // Maintain aspect ratio
        float srcAspect = sceneW > 0 ? (float)sceneW / (float)sceneH : 16.0f/9.0f;
        float w = avail.x, h = avail.x / srcAspect;
        if (h > avail.y) { h = avail.y; w = h * srcAspect; }
        ImVec2 offset = {(avail.x - w) * 0.5f, (avail.y - h) * 0.5f};
        ImGui::SetCursorPos({ImGui::GetCursorPos().x + offset.x,
                             ImGui::GetCursorPos().y + offset.y});
        ImGui::Image((ImTextureID)(uintptr_t)gameTex.idx, {w, h});
    }

    ImGui::End();
}
