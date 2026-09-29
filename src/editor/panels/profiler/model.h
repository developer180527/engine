#pragma once
// ── Profiler model — last frame's numbers, ready to draw, without a GUI ──────
// The instrumenting profiler (core/profiler.h) records scopes; a panel wants
// them as rows (name, depth, ms, % of frame), as flamegraph bars (0..1 across
// the frame, a stable colour per scope), a rolling frame-time history, and the
// memory counters. The ImGui Profiler and the libgui front end draw these.
#include "core/frame_arena.h"
#include "core/profiler.h"
#include "editor/core/color.h"
#include "runtime/mem_channel.h"

#include <cstdint>
#include <vector>

namespace profview {

struct PhaseRow { const char* name; int depth; double ms; double pct; };

inline std::vector<PhaseRow> phaseRows(const prof::TimerChannel& timer) {
    std::vector<PhaseRow> out;
    const double frameMs = timer.lastFrameMs();
    for (const auto& s : timer.lastFrame()) {
        if (s.threadIndex != 0) continue;          // main thread only
        const double ms = (s.end - s.start) / 1e6;
        out.push_back({s.name, s.depth, ms, frameMs > 0 ? 100.0 * ms / frameMs : 0.0});
    }
    return out;
}

struct FlameBar { const char* name; int depth; float x0, x1; edui::Color color; };

// Bars across the frame, x in 0..1; `maxDepth` is the deepest row used.
inline std::vector<FlameBar> flameBars(const prof::TimerChannel& timer, int& maxDepth) {
    std::vector<FlameBar> out;
    const uint64_t t0 = timer.lastFrameStart(), t1 = timer.lastFrameEnd();
    const double span = (t1 > t0) ? double(t1 - t0) : 1.0;
    maxDepth = 0;
    for (const auto& s : timer.lastFrame()) {
        if (s.threadIndex != 0 || s.end <= s.start) continue;
        if (s.depth > maxDepth) maxDepth = s.depth;
        // A stable colour per scope NAME (its string's address), so a phase
        // keeps its colour from frame to frame.
        const uint32_t h = (uint32_t)((uintptr_t)s.name >> 4);
        const edui::Color c{ (70 + (h * 53) % 160) / 255.0f, (70 + (h * 97) % 160) / 255.0f,
                             (70 + (h * 131) % 160) / 255.0f, 235 / 255.0f };
        out.push_back({s.name, s.depth, (float)((s.start - t0) / span),
                       (float)((s.end - t0) / span), c});
    }
    return out;
}

// The last N frame times, oldest first.
class FrameHistory {
public:
    static constexpr int kSize = 120;
    void push(float ms) { m_v[m_head] = ms; m_head = (m_head + 1) % kSize; }
    float at(int i) const { return m_v[(m_head + i) % kSize]; }   // 0 = oldest
    const float* raw() const { return m_v; }
    int  head() const { return m_head; }
private:
    float m_v[kSize] = {};
    int   m_head = 0;
};

struct Memory {
    bool     hasChannel = false;
    uint64_t cppAllocs = 0, cppFrees = 0, cppBytes = 0, flecsAllocs = 0, flecsFrees = 0;
    size_t   arenaUsed = 0, arenaCap = 0, arenaPeak = 0, arenaOverflow = 0;
};

inline Memory memory(prof::Profiler& profiler, mem::FrameArena* arena) {
    Memory m;
    for (auto* ch : profiler.channels())
        if (auto* mc = dynamic_cast<MemoryChannel*>(ch)) {
            m.hasChannel = true;
            m.cppAllocs = mc->cppAllocs(); m.cppFrees = mc->cppFrees(); m.cppBytes = mc->cppBytes();
            m.flecsAllocs = mc->flecsAllocs(); m.flecsFrees = mc->flecsFrees();
        }
    if (arena) {
        m.arenaUsed = arena->used(); m.arenaCap = arena->capacity();
        m.arenaPeak = arena->highWater(); m.arenaOverflow = arena->overflowBytes();
    }
    return m;
}

}  // namespace profview
