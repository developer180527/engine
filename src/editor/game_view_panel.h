#pragma once
#include <imgui.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <functional>
#include "engine_context.h"
#include "editor_state.h"
#include "components/camera.h"
#include "core/transform.h"
#include "core/transform_utils.h"

namespace detail_gv {
inline bx::Vec3 quatRotate(const bx::Quaternion& q, const bx::Vec3& v) {
    const bx::Vec3 qv  = {q.x, q.y, q.z};
    const bx::Vec3 uv  = bx::cross(qv, v);
    const bx::Vec3 uuv = bx::cross(qv, uv);
    return bx::add(v, bx::add(bx::mul(uv, 2.0f * q.w), bx::mul(uuv, 2.0f)));
}
} // namespace detail_gv

// Find primary camera in ANY world (editor or game)
inline bool findPrimaryCamera(flecs::world& world,
                              float view[16], float proj[16],
                              float aspect, float clearColor[4]) {
    bool found = false;
    world.query_builder<const Transform, const Camera>()
        .build()
        .each([&](flecs::entity e, const Transform& t, const Camera& c) {
            if (!c.isPrimary || found) return;
            found = true;
            // Use world matrix so parented cameras inherit parent transform.
            // Row-major bgfx layout:
            //   row0=[Rx,Ry,Rz,0]  row1=[Ux,Uy,Uz,0]
            //   row2=[Bx,By,Bz,0]  row3=[Tx,Ty,Tz,1]
            // Camera looks along -Z in local space, so world forward = -row2.
            float wm[16];
            getWorldMatrix(e, wm);
            bx::Vec3 pos = {wm[12], wm[13], wm[14]};
            bx::Vec3 fwd = bx::normalize({-wm[8], -wm[9], -wm[10]});
            bx::Vec3 up  = bx::normalize({ wm[4],  wm[5],  wm[6]});
            bx::mtxLookAt(view, pos, bx::add(pos, fwd), up);
            const bool rhNdc = bgfx::getCaps()->homogeneousDepth;
            if (c.projection == ProjectionType::Perspective)
                bx::mtxProj(proj, c.fov, aspect, c.nearPlane, c.farPlane, rhNdc);
            else {
                const float h = c.orthoSize, w = h * aspect;
                bx::mtxOrtho(proj, -w, w, -h, h, c.nearPlane, c.farPlane, 0.0f, rhNdc);
            }
            std::memcpy(clearColor, c.clearColor, 16);
        });
    return found;
}

// Play / Pause / Stop toolbar — drawn at top of Game View panel
inline void drawPlayBar(SimState state,
                        std::function<void()> onPlay,
                        std::function<void()> onPause,
                        std::function<void()> onStop) {
    // State indicator dot + label
    ImVec4 dotCol = {0.5f, 0.5f, 0.5f, 1.0f};
    const char* label = "EDITING";
    if (state == SimState::Playing) { dotCol = {0.2f, 0.9f, 0.3f, 1.0f}; label = "PLAYING"; }
    if (state == SimState::Paused)  { dotCol = {1.0f, 0.7f, 0.1f, 1.0f}; label = "PAUSED";  }

    ImGui::PushStyleColor(ImGuiCol_Text, dotCol);
    ImGui::TextUnformatted("●");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();

    // Center the three buttons
    float btnW = 52.0f, gap = 4.0f;
    float totalW = btnW * 3 + gap * 2;
    float avail  = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - totalW) * 0.5f);

    // Play / Resume
    bool canPlay  = (state == SimState::Editing || state == SimState::Paused);
    bool canPause = (state == SimState::Playing);
    bool canStop  = (state != SimState::Editing);

    if (!canPlay) ImGui::BeginDisabled();
    if (ImGui::Button("Play", {btnW, 0})) onPlay();
    if (!canPlay) ImGui::EndDisabled();
    ImGui::SameLine(0, gap);

    if (!canPause) ImGui::BeginDisabled();
    if (ImGui::Button("Pause", {btnW, 0})) onPause();
    if (!canPause) ImGui::EndDisabled();
    ImGui::SameLine(0, gap);

    if (!canStop) ImGui::BeginDisabled();
    if (state == SimState::Playing || state == SimState::Paused) {
        ImGui::PushStyleColor(ImGuiCol_Button,        {0.7f, 0.15f, 0.15f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.85f, 0.2f, 0.2f, 1.0f});
    }
    if (ImGui::Button("Stop", {btnW, 0})) onStop();
    if (state == SimState::Playing || state == SimState::Paused)
        ImGui::PopStyleColor(2);
    if (!canStop) ImGui::EndDisabled();

    ImGui::Separator();
}

inline void drawGameViewPanel(bgfx::TextureHandle gameTex, bool hasCam,
                              SimState simState, int sceneW, int sceneH,
                              std::function<void()> onPlay,
                              std::function<void()> onPause,
                              std::function<void()> onStop) {
    ImGui::Begin("Game View");

    drawPlayBar(simState, onPlay, onPause, onStop);

    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Green border tint during play
    if (simState == SimState::Playing) {
        ImGui::GetWindowDrawList()->AddRect(
            ImGui::GetWindowPos(),
            {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
             ImGui::GetWindowPos().y + ImGui::GetWindowSize().y},
            IM_COL32(40, 200, 60, 120), 0.0f, 0, 2.0f);
    }

    if (!hasCam) {
        const char* msg  = "No Camera in scene";
        const char* hint = "Hierarchy > + Add > Camera";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos({(avail.x - ts.x) * 0.5f + ImGui::GetStyle().WindowPadding.x,
                             avail.y * 0.4f});
        ImGui::TextDisabled("%s", msg);
        ts = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPosX((avail.x - ts.x) * 0.5f + ImGui::GetStyle().WindowPadding.x);
        ImGui::TextDisabled("%s", hint);
    } else if (!bgfx::isValid(gameTex)) {
        ImGui::TextDisabled("Initialising...");
    } else {
        float srcAspect = sceneW > 0 ? (float)sceneW / (float)sceneH : 16.0f/9.0f;
        float w = avail.x, h = avail.x / srcAspect;
        if (h > avail.y) { h = avail.y; w = h * srcAspect; }
        ImVec2 off = {(avail.x - w) * 0.5f, (avail.y - h) * 0.5f};
        ImGui::SetCursorPos({ImGui::GetCursorPos().x + off.x,
                             ImGui::GetCursorPos().y + off.y});
        ImGui::Image((ImTextureID)(uintptr_t)gameTex.idx, {w, h});
    }
    ImGui::End();
}
