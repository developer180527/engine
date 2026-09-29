#pragma once
// ── edui::Color — a colour a panel model decides, in no GUI's type ──────────
// Straight-alpha, sRGB-encoded 0..1 floats: what ImGui's ImVec4 and libgui's
// LibguiColor both take, so each front end converts with a copy. Models use
// this instead of ImVec4/ImU32 so they carry no GUI toolkit.
namespace edui {
struct Color { float r = 0, g = 0, b = 0, a = 1; };
}  // namespace edui
