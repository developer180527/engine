#pragma once
#include <imgui.h>
#include "editor/editor_icons.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <functional>
#include "editor/engine_context.h"
#include "editor_state.h"
#include "runtime/camera_util.h" // findPrimaryCamera

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
    ImGui::Begin(ICON_FA_GAMEPAD " Game View");

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
