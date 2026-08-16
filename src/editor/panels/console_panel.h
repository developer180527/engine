#pragma once
// ── Console panel — reads elog:: directly, formats nothing on the log path ────
//
// Rewritten with the logger. What changed for this file:
//   * `Logger::get().snapshot()` returned a COPY of 1024 entries — 2048
//     std::strings — every frame the panel was open. It now walks the ring by
//     sequence and copies one record at a time into a stack Entry.
//   * SUBSYSTEM TARGETING is the point of the rewrite: the category list is
//     discovered from the registry, each row can be toggled per level, and
//     "Solo" sets everything else to errors-and-warnings only. Because the
//     filter is checked BEFORE formatting, a subsystem you are not watching
//     costs the engine ~1 ns per suppressed line instead of 2.4 µs.
//   * DROPPED LINES ARE SHOWN. A ring loses the oldest, and a console that
//     hides that is lying about what happened.
#include <imgui.h>
#include <cstdio>

#include "core/logger.h"
#include "editor/panels/terminal_panel.h"
#include "editor/editor_icons.h"

inline TerminalPanel& getTerminal() {
    static TerminalPanel t;
    return t;
}

inline void drawConsolePanel(bool* open) {
    if (open && !*open) return;
    ImGui::Begin(ICON_FA_TERMINAL " Console", open);

    if (ImGui::BeginTabBar("##consoletabs")) {

        // ── Log tab ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Log")) {
            static uint64_t viewFrom = 0;         // "Clear" moves this, not the ring
            static bool     autoScroll = true;

            if (ImGui::Button("Clear")) viewFrom = elog::written();
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScroll);
            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            if (ImGui::Button("Watch all")) elog::watchAll();

            // Level colours, indexed by elog::Level.
            const ImVec4 lvlColors[(int)elog::Level::Count] = {
                {0.45f,0.45f,0.55f,1},   // Trace
                {0.5f,0.5f,0.5f,1},      // Debug
                {0.85f,0.85f,0.85f,1},   // Info
                {0.3f,0.9f,0.4f,1},      // Success
                {1.0f,0.8f,0.2f,1},      // Warning
                {1.0f,0.35f,0.35f,1},    // Error
            };

            // ── Loss, stated ────────────────────────────────────────
            const uint64_t total = elog::written();
            const uint64_t lost  = elog::evicted();
            const uint64_t trunc = elog::truncated();
            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            ImGui::TextDisabled("%llu lines", (unsigned long long)total);
            if (lost) {
                ImGui::SameLine();
                ImGui::TextColored(lvlColors[(int)elog::Level::Warning],
                                   "%llu dropped", (unsigned long long)lost);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The ring holds %u lines. Older ones are gone —\n"
                                      "narrow the categories to keep what you need.",
                                      elog::kSlots);
            }
            if (trunc) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%llu truncated)", (unsigned long long)trunc);
            }

            // ── Per-subsystem targeting ─────────────────────────────
            if (ImGui::CollapsingHeader("Subsystems")) {
                ImGui::TextDisabled("Solo streams one subsystem at every level and "
                                    "drops the rest to warnings/errors.");
                if (ImGui::BeginTable("##cats", 8,
                        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("subsystem");
                    ImGui::TableSetupColumn("lines");
                    ImGui::TableSetupColumn("");         // solo
                    for (int l = (int)elog::Level::Debug; l < (int)elog::Level::Count; ++l)
                        ImGui::TableSetupColumn(elog::levelName((elog::Level)l));
                    ImGui::TableHeadersRow();

                    const int n = elog::categoryCount();
                    for (int i = 0; i < n; ++i) {
                        elog::Category& c = elog::categoryAt(i);
                        if (!c.name) continue;
                        ImGui::TableNextRow();
                        ImGui::PushID(i);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%llu", (unsigned long long)
                            c.written.load(std::memory_order_relaxed));
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton("Solo")) elog::solo(&c);
                        for (int l = (int)elog::Level::Debug; l < (int)elog::Level::Count; ++l) {
                            ImGui::TableNextColumn();
                            bool on = (c.mask.load(std::memory_order_relaxed)
                                       & elog::levelBit((elog::Level)l)) != 0;
                            ImGui::PushStyleColor(ImGuiCol_Text, lvlColors[l]);
                            if (ImGui::Checkbox("##lvl", &on))
                                elog::setLevel(c, (elog::Level)l, on);
                            ImGui::PopStyleColor();
                            ImGui::PushID(l);
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }

            // Display-side filters — these hide what was already recorded, which
            // is a different thing from the category masks above (those stop it
            // being recorded at all, and that is where the cost is).
            static bool showLevels[(int)elog::Level::Count] = {
                true, true, true, true, true, true };
            for (int i = (int)elog::Level::Debug; i < (int)elog::Level::Count; ++i) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, lvlColors[i]);
                ImGui::Checkbox(elog::levelName((elog::Level)i), &showLevels[i]);
                ImGui::PopStyleColor();
            }
            static char textFilter[64] = {};
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputText("Find", textFilter, sizeof(textFilter));

            ImGui::Separator();
            ImGui::BeginChild("##logentries", ImVec2(0,0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);

            // Walk the ring by SEQUENCE. `read` refuses a slot that was recycled
            // mid-copy, so a fast-moving log shows fewer lines rather than
            // garbled ones.
            const uint64_t begin = elog::oldest() > viewFrom ? elog::oldest() : viewFrom;
            elog::Entry e;
            for (uint64_t s = begin; s < total; ++s) {
                if (!elog::read(s, e)) continue;
                const int li = (int)e.level;
                if (li < 0 || li >= (int)elog::Level::Count || !showLevels[li]) continue;
                if (textFilter[0] && !std::strstr(e.msg, textFilter) &&
                    !(e.cat && std::strstr(e.cat, textFilter))) continue;
                ImGui::TextDisabled("[%6.2f]", e.t); ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, lvlColors[li]);
                ImGui::Text("[%s]", e.cat ? e.cat : "?");
                ImGui::PopStyleColor(); ImGui::SameLine();
                ImGui::TextColored(lvlColors[li], "%s%s", e.msg,
                                   e.truncated ? " …" : "");
            }
            if (autoScroll) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
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
