#pragma once

#include <imgui.h>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>
#include <utility>
#include <algorithm>
#include <string>

#include "editor/engine_context.h"
#include "editor/undo_stack.h"

namespace inspector_detail {

// ── Quaternion <-> Euler conversion (UI boundary only) ──────────────────────
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

// ── Small colored circle — texture slot status indicator ────────────────────
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

// ── Section header with subtle background bar ───────────────────────────────
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

// ── Texture row: dot + label + filename (truncated) ─────────────────────────
inline void texRow(const char* slot, bool loaded, bool isNormal,
                   const std::string& name) {
    texDot(loaded, isNormal);
    ImGui::SameLine(0, 6);
    if (loaded) {
        ImGui::TextUnformatted(slot);
        if (!name.empty()) {
            ImGui::SameLine();
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

// ── Recursively find .lua files under projectRoot ───────────────────────────
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

// ── Shared continuous-widget undo helper ────────────────────────────────────
// Captures snapshot on ImGui::IsItemActivated(), pushes undo on
// IsItemDeactivatedAfterEdit(). Only one widget is active at a time in ImGui,
// so a single static pair is safe.
inline void propEdit(EngineContext& ctx, flecs::entity e,
                     const char* key, const char* desc) {
    static nlohmann::json s_propBefore;
    static bool           s_propCapturing = false;
    if (ImGui::IsItemActivated()) {
        s_propBefore = UndoStack::snapshotComponent(e, key);
        s_propCapturing = true;
    }
    if (s_propCapturing && ImGui::IsItemDeactivatedAfterEdit()) {
        auto after = UndoStack::snapshotComponent(e, key);
        if (s_propBefore != after)
            ctx.editor.undoStack.pushPropertyEdit(e, key, s_propBefore, after, desc);
        s_propCapturing = false;
        ctx.editor.sceneDirty = true;
    } else if (s_propCapturing && ImGui::IsItemDeactivated()) {
        s_propCapturing = false;
    }
}

} // namespace inspector_detail
