#pragma once
// ── RenderStatsChannel — profiler plumbing ONLY ─────────────────────────────
//
// This used to be a 151-line file doing four jobs: sampling bgfx counters,
// judging churn, holding budget knowledge, and formatting. Those are now one
// concern per file under render/diag/:
//
//   diag/frame_gpu_stats.h   sampling + churn verdict
//   diag/gpu_budget.h        target tiers and pass/fail policy
//   diag/resource_census.h   resident resources, owners, duplicates, leaks
//   diag/diag_report.h       formatting
//
// What is left here is the adapter that hangs sampling off the profiler's
// frame boundary. Callers that do not use the profiler (engine_player's
// --gpu-stats) drive FrameGpuStats directly; nothing forces them through this.
#include "core/profiler.h"
#include "render/diag/diag_report.h"
#include "render/diag/frame_gpu_stats.h"

class RenderStatsChannel final : public prof::IProfilerChannel {
public:
    const char* channelName() const override { return "RenderStats"; }
    void beginFrame() override {}
    void endFrame()   override { m_stats.sample(); }

    void report(const char* tag) const { rdiag::printFrameStats(m_stats, tag); }

    const rdiag::FrameGpuStats& stats() const { return m_stats; }

private:
    rdiag::FrameGpuStats m_stats;
};
