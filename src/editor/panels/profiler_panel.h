#pragma once
#include <imgui.h>
#include <cstdint>
#include <cstdio>

#include "core/profiler.h"
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
// (the same pattern as the plugins panel and IEditorPlugin).

namespace detail_prof {

// A simple flamegraph from the timer's last frame (thread 0). Each sample is a
// bar positioned by its start offset within the frame and stacked by depth.
inline void drawFlamegraph(const prof::TimerChannel& timer) {
    const auto& frame = timer.lastFrame();
    const uint64_t t0 = timer.lastFrameStart();
    const uint64_t t1 = timer.lastFrameEnd();
    const double span = (t1 > t0) ? double(t1 - t0) : 1.0;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  width  = ImGui::GetContentRegionAvail().x;
    const float  rowH   = 18.0f;

    int maxDepth = 0;
    for (const auto& s : frame)
        if (s.threadIndex == 0 && s.depth > maxDepth) maxDepth = s.depth;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& s : frame) {
        if (s.threadIndex != 0 || s.end <= s.start) continue;
        float x0 = origin.x + (float)((double)(s.start - t0) / span) * width;
        float x1 = origin.x + (float)((double)(s.end   - t0) / span) * width;
        if (x1 - x0 < 1.0f) x1 = x0 + 1.0f;
        const float y0 = origin.y + s.depth * rowH;
        // Stable per-name colour from the string-literal address.
        const uint32_t h = (uint32_t)((uintptr_t)s.name >> 4);
        const ImU32 col = IM_COL32(70 + (h * 53) % 160,
                                   70 + (h * 97) % 160,
                                   70 + (h * 131) % 160, 235);
        dl->AddRectFilled({x0, y0}, {x1, y0 + rowH - 2.0f}, col, 2.0f);
        if (x1 - x0 > 32.0f) {
            dl->PushClipRect({x0, y0}, {x1, y0 + rowH}, true);
            dl->AddText({x0 + 3.0f, y0 + 2.0f}, IM_COL32_WHITE, s.name);
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
    static float hist[120] = {0};
    static int   head = 0;
    const float  ms   = (float)timer.lastFrameMs();
    hist[head] = ms;
    head = (head + 1) % 120;
    char overlay[48];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms  (%.0f fps)",
                  ms, ms > 0.0f ? 1000.0f / ms : 0.0f);
    ImGui::PlotLines("##frametime", hist, 120, head, overlay,
                     0.0f, 33.3f, ImVec2(-1, 64));

    // ── CPU: per-phase table (parent-ID tree → depth indent) ────────────────
    if (ImGui::CollapsingHeader("CPU — frame phases", ImGuiTreeNodeFlags_DefaultOpen)) {
        const double frameMs = timer.lastFrameMs();
        if (ImGui::BeginTable("##timers", 3,
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("%",  ImGuiTableColumnFlags_WidthFixed, 52);
            ImGui::TableHeadersRow();
            for (const auto& s : timer.lastFrame()) {
                if (s.threadIndex != 0) continue;
                const double sms = (s.end - s.start) / 1e6;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (s.depth) ImGui::Indent(s.depth * 12.0f);
                ImGui::TextUnformatted(s.name);
                if (s.depth) ImGui::Unindent(s.depth * 12.0f);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", sms);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%", frameMs > 0 ? 100.0 * sms / frameMs : 0.0);
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
        MemoryChannel* mem = nullptr;
        for (auto* ch : profiler.channels())
            if (auto* m = dynamic_cast<MemoryChannel*>(ch)) mem = m;
        if (mem) {
            ImGui::Text("C++   new %llu  free %llu  (%llu B)",
                        (unsigned long long)mem->cppAllocs(),
                        (unsigned long long)mem->cppFrees(),
                        (unsigned long long)mem->cppBytes());
            ImGui::Text("flecs alloc %llu  free %llu",
                        (unsigned long long)mem->flecsAllocs(),
                        (unsigned long long)mem->flecsFrees());
        } else {
            ImGui::TextDisabled("No memory channel registered");
        }
        ImGui::Separator();
        const size_t used = arena.used(), cap = arena.capacity(), peak = arena.highWater();
        ImGui::Text("Frame arena: %zu / %zu KB   peak %zu KB",
                    used / 1024, cap / 1024, peak / 1024);
        ImGui::ProgressBar(cap ? (float)used / (float)cap : 0.0f, ImVec2(-1, 0));
        if (arena.overflowBytes())
            ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f},
                "arena overflowed to heap (%zu B) — raise size",
                arena.overflowBytes());
    }

    ImGui::End();
}
