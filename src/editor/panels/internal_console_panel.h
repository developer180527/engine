#pragma once
// ── Internal Console — for the person debugging the ENGINE ───────────────────
//
// The other console (`console_panel.h`) is for someone building a game: their
// content, their scripts, and any failure anywhere. This one is the instrument
// for the machinery — extraction phases, the job pool, allocator growth, the
// asset cache — and it exists as a separate panel rather than a tab because the
// two audiences want opposite things. A game developer wants the noise gone; an
// engine developer wants to aim at one subsystem and watch it stream.
//
// OFF BY DEFAULT (View > Panels > Internal Console). A game developer should
// never have to know this panel exists.
//
// What it adds over the game console:
//   * TARGETING. One click on Solo streams a subsystem at every level and drops
//     everyone else to warnings and errors. Because the mask is checked BEFORE
//     the message is formatted, the subsystems you are not watching cost the
//     engine ~0.7 ns per suppressed line — which is what makes it usable while
//     the game runs, rather than a thing you enable and then measure nothing.
//   * The RING's own state: how full, how many lines were evicted, how many
//     truncated. A diagnostic tool that silently drops what you are hunting is
//     worse than no tool.
//   * Per-category volume, so "who is flooding this?" is answerable.
#include <imgui.h>
#include <cstring>

#include "core/logger.h"
#include "editor/panels/console_panel.h"   // elogLevelColors()
#include "editor/editor_icons.h"

inline void drawInternalConsolePanel(bool* open) {
    if (open && !*open) return;
    ImGui::Begin(ICON_FA_BUG " Internal Console", open);

    const ImVec4* col = elogLevelColors();
    static uint64_t viewFrom   = 0;
    static bool     autoScroll = true;
    static bool     onlyEngine = false;   // hide game-facing chatter

    // ── Ring health ─────────────────────────────────────────────────────────
    const uint64_t total   = elog::written();
    const uint64_t lost    = elog::evicted();
    const uint64_t trunc   = elog::truncated();
    const uint64_t inRing  = total - elog::oldest();

    if (ImGui::Button("Clear")) viewFrom = elog::written();
    ImGui::SameLine(); ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::SameLine(); ImGui::Checkbox("Engine only", &onlyEngine);
    ImGui::SameLine();
    if (ImGui::Button("Watch all")) elog::watchAll();
    ImGui::SameLine();
    {
        static bool toStdout = true;
        if (ImGui::Checkbox("stdout", &toStdout)) elog::setStdout(toStdout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mirror to the terminal. Off costs ~0.12 us a line\n"
                              "and is what a shipping build wants.");
    }

    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::TextDisabled("ring %llu/%u", (unsigned long long)inRing, elog::kSlots);
    if (lost) {
        ImGui::SameLine();
        ImGui::TextColored(col[(int)elog::Level::Warning], "%llu evicted",
                           (unsigned long long)lost);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lines that scrolled out before anyone read them.\n"
                              "Exact, not an estimate. Solo a subsystem to keep\n"
                              "what you are actually looking for.");
    }
    if (trunc) {
        ImGui::SameLine();
        ImGui::TextDisabled("%llu truncated (>%u chars)",
                            (unsigned long long)trunc, elog::kMsgMax - 1);
    }

    // ── Subsystem targeting ─────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Subsystems", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Solo: stream one subsystem at every level, drop the "
                            "rest to warnings/errors. Unchecked levels are never "
                            "formatted, so they cost ~0.7 ns.");
        const int nLevels = (int)elog::Level::Count - (int)elog::Level::Trace;
        if (ImGui::BeginTable("##cats", 4 + nLevels,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                ImVec2(0, 180))) {
            ImGui::TableSetupColumn("subsystem");
            ImGui::TableSetupColumn("for");
            ImGui::TableSetupColumn("lines");
            ImGui::TableSetupColumn("");                    // solo
            for (int l = (int)elog::Level::Trace; l < (int)elog::Level::Count; ++l)
                ImGui::TableSetupColumn(elog::levelName((elog::Level)l));
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            const int n = elog::categoryCount();
            for (int i = 0; i < n; ++i) {
                elog::Category& c = elog::categoryAt(i);
                if (!c.name) continue;
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.name);
                ImGui::TableNextColumn();
                ImGui::TextDisabled(elog::audienceOf(c) == elog::Audience::Game
                                    ? "game" : "engine");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%llu", (unsigned long long)
                                    c.written.load(std::memory_order_relaxed));
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Solo")) elog::solo(&c);

                for (int l = (int)elog::Level::Trace; l < (int)elog::Level::Count; ++l) {
                    ImGui::TableNextColumn();
                    ImGui::PushID(l);
                    bool on = (c.mask.load(std::memory_order_relaxed)
                               & elog::levelBit((elog::Level)l)) != 0;
                    ImGui::PushStyleColor(ImGuiCol_Text, col[l]);
                    if (ImGui::Checkbox("##lvl", &on))
                        elog::setLevel(c, (elog::Level)l, on);
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
#if !defined(ENGINE_LOG_TRACE)
        ImGui::TextDisabled("(trace is compiled out — build with ENGINE_LOG_TRACE "
                            "to enable that column)");
#endif
    }

    // ── Display-side filters ────────────────────────────────────────────────
    // Distinct from the masks above: these hide what was already recorded. The
    // masks stop it being recorded at all, and that is where the cost is.
    static bool show[(int)elog::Level::Count] = { true,true,true,true,true,true };
    for (int i = 0; i < (int)elog::Level::Count; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, col[i]);
        ImGui::Checkbox(elog::levelName((elog::Level)i), &show[i]);
        ImGui::PopStyleColor();
    }
    static char find[64] = {};
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Find", find, sizeof(find));

    ImGui::Separator();
    ImGui::BeginChild("##internallog", ImVec2(0,0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const uint64_t begin = elog::oldest() > viewFrom ? elog::oldest() : viewFrom;
    elog::Entry e;
    for (uint64_t s = begin; s < total; ++s) {
        // `read` refuses a slot a writer recycled mid-copy, so a flooding
        // subsystem shows fewer lines here rather than spliced ones.
        if (!elog::read(s, e)) continue;
        const int li = (int)e.level;
        if (li < 0 || li >= (int)elog::Level::Count || !show[li]) continue;
        if (onlyEngine && elog::visibleToGame(
                elog::category(e.cat ? e.cat : "?"), elog::Level::Info) &&
            e.level < elog::Level::Warning) continue;
        if (find[0] && !std::strstr(e.msg, find) &&
            !(e.cat && std::strstr(e.cat, find))) continue;
        // Frame number, which the game console omits: "which frame" is the
        // question an engine bug is actually about.
        ImGui::TextDisabled("[%6.2f|f%llu]", e.t, (unsigned long long)e.frame);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, col[li]);
        ImGui::Text("[%s]", e.cat ? e.cat : "?");
        ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::TextColored(col[li], "%s%s", e.msg, e.truncated ? " …" : "");
    }
    if (autoScroll) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}
