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
#include <cstdint>
#include <cstring>

#include "core/logger.h"
#include "editor/panels/console/model.h"
#include "editor/panels/terminal_panel.h"
#include "editor/editor_icons.h"

inline TerminalPanel& getTerminal() {
    static TerminalPanel t;
    return t;
}

// Shared with the internal console so the two panels agree on colour per
// level; the colours themselves are the console model's.
inline const ImVec4* elogLevelColors() {
    static ImVec4 c[(int)elog::Level::Count];
    static const bool init = [] {
        for (int i = 0; i < (int)elog::Level::Count; ++i) {
            const edui::Color k = console::levelColor((elog::Level)i);
            c[i] = ImVec4(k.r, k.g, k.b, k.a);
        }
        return true;
    }();
    (void)init;
    return c;
}

// One log line, as both consoles print it.
inline void drawLogLine(const console::Line& l, bool withFrame) {
    const ImVec4 col = elogLevelColors()[(int)l.level];
    if (withFrame) ImGui::TextDisabled("[%6.2f|f%llu]", l.t, (unsigned long long)l.frame);
    else           ImGui::TextDisabled("[%6.2f]", l.t);
    ImGui::SameLine();
    ImGui::TextColored(col, "[%s]", l.cat.c_str());
    ImGui::SameLine();
    ImGui::TextColored(col, "%s", l.msg.c_str());
}

inline void drawConsolePanel(bool* open) {
    if (open && !*open) return;
    ImGui::Begin(ICON_FA_TERMINAL " Console", open);

    if (ImGui::BeginTabBar("##consoletabs")) {

        // ── Log tab ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Log")) {
            static console::LogView view(console::Audience::Game);
            static bool autoScroll = true;
            const ImVec4* col = elogLevelColors();

            if (ImGui::Button("Clear")) view.clear();
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScroll);

            // Levels a game builder cares about. Debug/Trace are engine-side and
            // are not offered here at all.
            ImGui::SameLine(); ImGui::TextDisabled("|");
            for (int i = (int)elog::Level::Info; i < (int)elog::Level::Count; ++i) {
                ImGui::SameLine();
                bool on = view.showing((elog::Level)i);
                ImGui::PushStyleColor(ImGuiCol_Text, col[i]);
                if (ImGui::Checkbox(elog::levelName((elog::Level)i), &on)) view.setShowing((elog::Level)i, on);
                ImGui::PopStyleColor();
            }

            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            static char find[64] = {};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("Find", find, sizeof(find))) view.find() = find;

            const console::LogView::Result r = view.collect(SIZE_MAX);

            ImGui::Separator();
            ImGui::BeginChild("##gamelog", ImVec2(0,0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& l : r.lines) drawLogLine(l, /*withFrame*/ false);
            if (autoScroll) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            // Counts, so "no errors" is a statement rather than an absence.
            if (r.errors || r.warnings) {
                ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 190.0f, 30.0f));
                if (r.errors) {
                    ImGui::TextColored(col[(int)elog::Level::Error],
                                       ICON_FA_TERMINAL " %u error%s", r.errors,
                                       r.errors == 1 ? "" : "s");
                    if (r.warnings) ImGui::SameLine();
                }
                if (r.warnings)
                    ImGui::TextColored(col[(int)elog::Level::Warning], "%u warning%s",
                                       r.warnings, r.warnings == 1 ? "" : "s");
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
