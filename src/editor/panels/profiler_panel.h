#pragma once
#include <imgui.h>
#include <cstdint>
#include <cstdio>

#include "core/profiler.h"
#include "editor/panels/profiler/model.h"
#include "core/frame_arena.h"
#include "runtime/mem_channel.h"
#include "editor/editor_icons.h"

// ── Profiler panel (editor overlay) ─────────────────────────────────────────
// Walks the profiler's channel registry and renders each known channel:
//   • Timer  — rolling frame-time graph + per-phase table (parent-ID tree) +
//              a simple flamegraph.
//   • Memory — per-frame C++ / flecs allocation counts + the frame arena's
//              live usage and high-water mark.
// Pure editor-layer; downcasts to the channel types it knows how to draw
// (the same pattern as the plugins panel).

namespace detail_prof {

// A simple flamegraph from the timer's last frame (thread 0). Each sample is a
// bar positioned by its start offset within the frame and stacked by depth.
inline void drawFlamegraph(const prof::TimerChannel& timer) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const float  rowH   = 18.0f;
    int maxDepth = 0;
    const auto bars = profview::flameBars(timer, maxDepth);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& b : bars) {
        float x0 = origin.x + b.x0 * width, x1 = origin.x + b.x1 * width;
        if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
        const float y0 = origin.y + b.depth * rowH;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(b.color.r, b.color.g, b.color.b, b.color.a));
        dl->AddRectFilled({x0, y0}, {x1, y0 + rowH - 2.0f}, col, 2.0f);
        if (x1 - x0 > 32.0f) {
            dl->PushClipRect({x0, y0}, {x1, y0 + rowH}, true);
            dl->AddText({x0 + 3.0f, y0 + 2.0f}, IM_COL32_WHITE, b.name);
            dl->PopClipRect();
        }
    }
    ImGui::Dummy(ImVec2(width, (maxDepth + 1) * rowH + 4.0f)); // reserve layout space
}

} // namespace detail_prof

inline void drawProfilerPanel(bool* open, mem::FrameArena& arena) {
    if (!open || !*open) return;
    if (!ImGui::Begin(ICON_FA_CHART_LINE " Profiler", open)) { ImGui::End(); return; }

    auto& profiler = prof::Profiler::get();
    auto& timer    = profiler.timer();

    bool enabled = profiler.enabled();
    if (ImGui::Checkbox("Enabled", &enabled)) profiler.setEnabled(enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("instrumenting profiler — phase granularity");

    // ── Rolling frame-time graph ────────────────────────────────────────────
    static profview::FrameHistory hist;
    const float ms = (float)timer.lastFrameMs();
    hist.push(ms);
    char overlay[48];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms  (%.0f fps)",
                  ms, ms > 0.0f ? 1000.0f / ms : 0.0f);
    ImGui::PlotLines("##frametime", hist.raw(), profview::FrameHistory::kSize, hist.head(), overlay,
                     0.0f, 33.3f, ImVec2(-1, 64));

    // ── CPU: per-phase table (parent-ID tree → depth indent) ────────────────
    if (ImGui::CollapsingHeader("CPU — frame phases", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##timers", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("%",  ImGuiTableColumnFlags_WidthFixed, 52);
            ImGui::TableHeadersRow();
            for (const auto& r : profview::phaseRows(timer)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (r.depth) ImGui::Indent(r.depth * 12.0f);
                ImGui::TextUnformatted(r.name);
                if (r.depth) ImGui::Unindent(r.depth * 12.0f);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", r.ms);
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%", r.pct);
            }
            ImGui::EndTable();
        }
        if (timer.lastFrameDropped())
            ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f},
                "%llu sample(s) dropped this frame (overflow)",
                (unsigned long long)timer.lastFrameDropped());
    }

    // ── Flamegraph ──────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Flamegraph"))
        detail_prof::drawFlamegraph(timer);

    // ── Memory ──────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        const profview::Memory m = profview::memory(profiler, &arena);
        if (m.hasChannel) {
            ImGui::Text("C++   new %llu  free %llu  (%llu B)",
                        (unsigned long long)m.cppAllocs, (unsigned long long)m.cppFrees,
                        (unsigned long long)m.cppBytes);
            ImGui::Text("flecs alloc %llu  free %llu",
                        (unsigned long long)m.flecsAllocs, (unsigned long long)m.flecsFrees);
        } else {
            ImGui::TextDisabled("No memory channel registered");
        }
        ImGui::Separator();
        ImGui::Text("Frame arena: %zu / %zu KB   peak %zu KB",
                    m.arenaUsed / 1024, m.arenaCap / 1024, m.arenaPeak / 1024);
        ImGui::ProgressBar(m.arenaCap ? (float)m.arenaUsed / (float)m.arenaCap : 0.0f, ImVec2(-1, 0));
        if (m.arenaOverflow)
            ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f},
                "arena overflowed to heap (%zu B) — raise size", m.arenaOverflow);
    }

    ImGui::End();
}
