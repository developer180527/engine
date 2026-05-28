#pragma once
#include <imgui.h>

// ── Matte-black editor theme ───────────────────────────────────────────────
// Premiere-Pro-inspired: neutral near-black panels (no blue tint), compact
// refined spacing, minimal rounding, a single muted steel-blue accent for
// selection / active states. Call AFTER ImGui::StyleColorsDark() so any color
// not explicitly set keeps a sane fallback.
//
// To re-skin the whole editor, change kAccent below — it drives selection,
// active sliders, checkmarks, tab overline, drag-drop and nav highlights.
inline void applyEditorTheme(float uiScale = 1.0f) {
    ImGuiStyle& s = ImGui::GetStyle();

    // Single accent — tweak this one line to recolor the editor.
    const ImVec4 kAccent     = ImVec4(0.23f, 0.49f, 0.80f, 1.00f); // steel blue
    const ImVec4 kAccentSoft = ImVec4(0.23f, 0.49f, 0.80f, 0.55f);

    auto g = [](float v, float a = 1.0f) { return ImVec4(v, v, v, a); };

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = g(0.86f);
    c[ImGuiCol_TextDisabled]          = g(0.40f);
    c[ImGuiCol_WindowBg]              = g(0.072f);          // matte black
    c[ImGuiCol_ChildBg]               = g(0.072f);
    c[ImGuiCol_PopupBg]               = g(0.135f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(1,1,1,0.06f);
    c[ImGuiCol_BorderShadow]          = g(0.0f, 0.0f);
    c[ImGuiCol_FrameBg]               = g(0.180f);          // inputs / sliders
    c[ImGuiCol_FrameBgHovered]        = g(0.235f);
    c[ImGuiCol_FrameBgActive]         = g(0.275f);
    c[ImGuiCol_TitleBg]               = g(0.090f);
    c[ImGuiCol_TitleBgActive]         = g(0.120f);
    c[ImGuiCol_TitleBgCollapsed]      = g(0.090f);
    c[ImGuiCol_MenuBarBg]             = g(0.130f);
    c[ImGuiCol_ScrollbarBg]           = g(0.075f);
    c[ImGuiCol_ScrollbarGrab]         = g(0.260f);
    c[ImGuiCol_ScrollbarGrabHovered]  = g(0.320f);
    c[ImGuiCol_ScrollbarGrabActive]   = g(0.380f);
    c[ImGuiCol_CheckMark]             = kAccent;
    c[ImGuiCol_SliderGrab]            = g(0.500f);
    c[ImGuiCol_SliderGrabActive]      = kAccent;
    c[ImGuiCol_Button]                = g(0.200f);
    c[ImGuiCol_ButtonHovered]         = g(0.270f);
    c[ImGuiCol_ButtonActive]          = g(0.320f);
    c[ImGuiCol_Header]                = kAccentSoft;        // selected rows / trees
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.23f,0.49f,0.80f,0.75f);
    c[ImGuiCol_HeaderActive]          = kAccent;
    c[ImGuiCol_Separator]             = ImVec4(1,1,1,0.08f);
    c[ImGuiCol_SeparatorHovered]      = kAccentSoft;
    c[ImGuiCol_SeparatorActive]       = kAccent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(1,1,1,0.05f);
    c[ImGuiCol_ResizeGripHovered]     = kAccentSoft;
    c[ImGuiCol_ResizeGripActive]      = kAccent;
    c[ImGuiCol_Tab]                   = g(0.130f);
    c[ImGuiCol_TabHovered]            = g(0.270f);
    c[ImGuiCol_TabSelected]           = g(0.200f);
    c[ImGuiCol_TabSelectedOverline]   = kAccent;            // accent line atop active tab
    c[ImGuiCol_TabDimmed]             = g(0.110f);
    c[ImGuiCol_TabDimmedSelected]     = g(0.160f);
    c[ImGuiCol_DockingPreview]        = kAccentSoft;
    c[ImGuiCol_DockingEmptyBg]        = g(0.075f);
    c[ImGuiCol_PlotLines]             = g(0.60f);
    c[ImGuiCol_PlotLinesHovered]      = kAccent;
    c[ImGuiCol_PlotHistogram]         = kAccent;
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.33f,0.59f,0.90f,1.00f);
    c[ImGuiCol_TableHeaderBg]         = g(0.160f);
    c[ImGuiCol_TableBorderStrong]     = g(0.220f);
    c[ImGuiCol_TableBorderLight]      = g(0.140f);
    c[ImGuiCol_TableRowBg]            = g(0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1,1,1,0.022f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.23f,0.49f,0.80f,0.35f);
    c[ImGuiCol_NavCursor]             = kAccent;
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.95f,0.65f,0.10f,0.90f); // amber, stands out
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0,0,0,0.55f);

    // ── Geometry — compact, minimal rounding (Premiere is mostly square) ────
    s.WindowPadding      = ImVec2(8, 8);
    s.FramePadding       = ImVec2(8, 4);
    s.CellPadding        = ImVec2(6, 4);
    s.ItemSpacing        = ImVec2(8, 6);
    s.ItemInnerSpacing   = ImVec2(6, 4);
    s.IndentSpacing      = 18.0f;
    s.ScrollbarSize      = 12.0f;
    s.GrabMinSize        = 9.0f;

    s.WindowBorderSize   = 1.0f;
    s.ChildBorderSize    = 1.0f;
    s.PopupBorderSize    = 1.0f;
    s.FrameBorderSize    = 0.0f;
    s.TabBarBorderSize   = 1.0f;

    s.WindowRounding     = 0.0f;
    s.ChildRounding      = 2.0f;
    s.FrameRounding      = 3.0f;
    s.PopupRounding      = 3.0f;
    s.ScrollbarRounding  = 3.0f;
    s.GrabRounding       = 3.0f;
    s.TabRounding        = 3.0f;

    s.WindowTitleAlign   = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None; // hide the collapse arrow — cleaner
    s.SeparatorTextBorderSize  = 1.0f;
    s.SeparatorTextPadding     = ImVec2(18, 4);

    if (uiScale != 1.0f) s.ScaleAllSizes(uiScale);

    // Viewports require opaque window backgrounds.
    s.Colors[ImGuiCol_WindowBg].w = 1.0f;
}
