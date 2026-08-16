#pragma once
// ── Console — for the person building a GAME ─────────────────────────────────
//
// This panel answers one question: is anything wrong with MY content or MY
// script? So it shows game-facing categories at every level, plus warnings and
// errors from everywhere (`elog::visibleToGame`) — a hard failure must never be
// filed under "engine internals" and hidden from the person whose build is
// broken.
//
// What it deliberately does NOT show: extraction phase timings, job pool state,
// allocator growth, per-category masks. Those belong to whoever is debugging the
// ENGINE, and they have their own panel — `internal_console_panel.h`. The two
// read the same ring; only the filter and the instrumentation differ.
//
// That separation was a correction. The first version of this rewrite put the
// subsystem grid in here, which hands a game developer a diagnostic console for
// somebody else's problem and buries theirs in it.
#include <imgui.h>
#include <cstring>

#include "core/logger.h"
#include "editor/panels/terminal_panel.h"
#include "editor/editor_icons.h"

inline TerminalPanel& getTerminal() {
    static TerminalPanel t;
    return t;
}

// Shared with the internal console so the two panels agree on colour per level.
inline const ImVec4* elogLevelColors() {
    static const ImVec4 c[(int)elog::Level::Count] = {
        {0.45f,0.45f,0.55f,1},   // Trace
        {0.5f, 0.5f, 0.5f, 1},   // Debug
        {0.85f,0.85f,0.85f,1},   // Info
        {0.3f, 0.9f, 0.4f, 1},   // Success
        {1.0f, 0.8f, 0.2f, 1},   // Warning
        {1.0f, 0.35f,0.35f,1},   // Error
    };
    return c;
}

inline void drawConsolePanel(bool* open) {
    if (open && !*open) return;
    ImGui::Begin(ICON_FA_TERMINAL " Console", open);

    if (ImGui::BeginTabBar("##consoletabs")) {

        // ── Log tab ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Log")) {
            static uint64_t viewFrom   = 0;    // "Clear" moves the view, not the ring
            static bool     autoScroll = true;
            const ImVec4*   col        = elogLevelColors();

            if (ImGui::Button("Clear")) viewFrom = elog::written();
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScroll);

            // Levels a game builder cares about. Debug/Trace are engine-side and
            // are not offered here at all.
            ImGui::SameLine(); ImGui::TextDisabled("|");
            static bool show[(int)elog::Level::Count] = { false,false,true,true,true,true };
            for (int i = (int)elog::Level::Info; i < (int)elog::Level::Count; ++i) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col[i]);
                ImGui::Checkbox(elog::levelName((elog::Level)i), &show[i]);
                ImGui::PopStyleColor();
            }

            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            static char find[64] = {};
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("Find", find, sizeof(find));

            // Counts, so "no errors" is a statement rather than an absence.
            const uint64_t total = elog::written();
            uint32_t warns = 0, errs = 0;

            ImGui::Separator();
            ImGui::BeginChild("##gamelog", ImVec2(0,0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);

            const uint64_t begin = elog::oldest() > viewFrom ? elog::oldest() : viewFrom;
            elog::Entry e;
            for (uint64_t s = begin; s < total; ++s) {
                if (!elog::read(s, e)) continue;
                const int li = (int)e.level;
                if (li < 0 || li >= (int)elog::Level::Count) continue;
                // THE audience rule. Everything else in this panel is chrome.
                if (!elog::visibleToGame(elog::category(e.cat ? e.cat : "?"), e.level))
                    continue;
                if (e.level == elog::Level::Warning) ++warns;
                if (e.level == elog::Level::Error)   ++errs;
                if (!show[li]) continue;
                if (find[0] && !std::strstr(e.msg, find) &&
                    !(e.cat && std::strstr(e.cat, find))) continue;
                ImGui::TextDisabled("[%6.2f]", e.t); ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col[li]);
                ImGui::Text("[%s]", e.cat ? e.cat : "?");
                ImGui::PopStyleColor(); ImGui::SameLine();
                ImGui::TextColored(col[li], "%s%s", e.msg, e.truncated ? " …" : "");
            }
            if (autoScroll) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            // Drawn after the loop so the counts are this frame's.
            if (errs || warns) {
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 190.0f, 30.0f));
                if (errs) {
                    ImGui::TextColored(col[(int)elog::Level::Error],
                                       ICON_FA_TERMINAL " %u error%s", errs,
                                       errs == 1 ? "" : "s");
                    if (warns) ImGui::SameLine();
                }
                if (warns)
                    ImGui::TextColored(col[(int)elog::Level::Warning], "%u warning%s",
                                       warns, warns == 1 ? "" : "s");
            }
            ImGui::EndTabItem();
        }

        // ── Terminal tab ─────────────────────────────────────────────
        if (ImGui::BeginTabItem("Terminal")) {
            getTerminal().draw();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}
