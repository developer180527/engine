#pragma once
// ── window_chrome — the title bar the OS no longer draws ─────────────────────
// Shared by every top-level page the editor shows, because "the window has no
// title bar" is a property of the WINDOW, not of whichever screen happens to be
// up. The project hub is a different page from the editor proper and needs the
// identical band: same height, same inset past the window buttons, same drag
// and double-click behaviour. Keeping this in one place is what stops the hub
// quietly losing the ability to move the window — which is exactly what
// happened when only the menu bar had it.
#include <functional>
#include <imgui.h>

#include "runtime/platform/title_bar.h"

namespace edchrome {

// Width of one window button, and the total the buttons occupy. Zero where the
// OS still draws its own (macOS), so callers reserve nothing.
inline float windowButtonWidth() { return 46.0f; }
inline float windowButtonsWidth() {
    return platwin::titleBarNeedsCustomButtons() ? 3.0f * windowButtonWidth()
                                                 : 0.0f;
}

// The three window buttons, drawn rather than fonted: glyphs would need Segoe
// MDL2 Assets (Windows-only, absent on the machines this is developed on), so
// they are primitives. Sized to the Windows convention — 46 wide, 10px glyph —
// because that is what every other window on the user's desktop uses.
inline void drawWindowButtons(float barHeight,
                              const std::function<void()>& onMinimize,
                              const std::function<void()>& onZoom,
                              const std::function<void()>& onClose) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w  = windowButtonWidth();
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);

    struct Btn { const char* id; int kind; };   // 0=min 1=max 2=close
    const Btn btns[] = { {"##win_min",0}, {"##win_max",1}, {"##win_close",2} };

    for (const Btn& b : btns) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(b.id, ImVec2(w, barHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool held    = ImGui::IsItemActive();

        if (hovered) {
            // Close goes red, the others grey — the convention on every
            // Windows window, and the affordance that stops someone closing
            // the editor when they meant to maximise it.
            const ImU32 bg = b.kind == 2
                ? IM_COL32(196, 43, 28, held ? 200 : 255)
                : ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                          : ImGuiCol_ButtonHovered);
            dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + barHeight), bg);
        }

        const ImVec2 c(p0.x + w * 0.5f, p0.y + barHeight * 0.5f);
        const float  h = 5.0f;                  // half of the 10px glyph
        switch (b.kind) {
        case 0:
            dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), fg, 1.0f);
            break;
        case 1:
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h),
                        fg, 0.0f, 0, 1.0f);
            break;
        default:
            dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), fg, 1.0f);
            dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h, c.y + h), fg, 1.0f);
            break;
        }

        if (ImGui::IsItemDeactivated() && hovered) {
            if      (b.kind == 0 && onMinimize) onMinimize();
            else if (b.kind == 1 && onZoom)     onZoom();
            else if (b.kind == 2 && onClose)    onClose();
        }
        ImGui::SameLine(0.0f, 0.0f);            // buttons abut, no gap
    }
}

// A stretch of empty title-bar standing in for the strip the window manager
// used to own: drag to move, double-click to zoom. `width` is how much space
// to claim on the current line.
//
// An InvisibleButton rather than a hover-rect test, deliberately: the button
// takes ownership of the press, so a drag that starts here stays ours after the
// pointer leaves the strip, and it cannot be confused with a click that began
// in a panel below.
inline void dragStrip(const char* id, float width, float height,
                      void* nativeWindow) {
    if (width <= 1.0f) return;
    ImGui::InvisibleButton(id, ImVec2(width, height));
    // Zoom first: a double-click also reports active+dragging on its second
    // press, so testing drag first would nudge the window and eat the gesture.
    if (ImGui::IsItemHovered()
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        platwin::toggleWindowZoom(nativeWindow);
    } else if (ImGui::IsItemActive()
               && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        // The OS runs the move loop from here, so this fires once per gesture;
        // the item staying active until release is the guard against re-entry.
        platwin::beginWindowDrag(nativeWindow);
    }
}

} // namespace edchrome
