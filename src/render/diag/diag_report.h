#pragma once
// ── diag_report — human-readable rendering of the diagnostics ───────────────
//
// ONE concern: formatting. Kept out of frame_gpu_stats / gpu_budget /
// resource_census so those stay pure logic with no I/O, which is what lets
// them be unit-tested headlessly. Nothing here decides anything; it only
// prints what the others computed.
#include "render/diag/frame_gpu_stats.h"
#include "render/diag/gpu_budget.h"
#include "render/diag/resource_census.h"

#include <cstdio>

namespace rdiag {

inline void printFrameStats(const FrameGpuStats& s, const char* tag) {
    if (s.frames() == 0) {
        std::printf("[RenderStats] %s — no frames sampled\n", tag);
        return;
    }
    const double mb = 1.0 / (1024.0 * 1024.0);
    std::printf(
        "[RenderStats] %s — %llu frames\n"
        "      draws        avg %.1f   max %u\n"
        "      VRAM         tex %.1f MB   rt %.1f MB   peak %.1f MB\n"
        "      transient    vb %.1f KB/frame   ib %.1f KB/frame\n"
        "      handles      %d live (tex %u vb %u ib %u fb %u prog %u uni %u)\n"
        "      CHURN        %llu/%llu frames changed handle counts -> %s\n",
        tag, (unsigned long long)s.frames(),
        s.avgDraws(), s.maxDraws(),
        (double)s.textureBytes() * mb, (double)s.rtBytes() * mb,
        (double)s.peakVramBytes() * mb,
        s.avgTransientVbKb(), s.avgTransientIbKb(),
        s.lastCounts().total(), s.lastCounts().textures,
        s.lastCounts().vertexBuffers, s.lastCounts().indexBuffers,
        s.lastCounts().frameBuffers, s.lastCounts().programs,
        s.lastCounts().uniforms,
        (unsigned long long)s.churnFrames(), (unsigned long long)s.frames(),
        toString(s.churn()));

    // Timing last, because it is the number that decides whether any of the
    // counts above are worth acting on: draws say how much we ASKED the GPU for,
    // GPU time says what it cost.
    if (s.gpuTimedFrames() > 0)
        std::printf("      GPU          avg %.2f ms   max %.2f ms  (%llu timed frame(s))\n",
                    s.avgGpuMs(), s.maxGpuMs(),
                    (unsigned long long)s.gpuTimedFrames());
    else
        std::printf("      GPU          unavailable — this backend has no GPU "
                    "timer (reporting 0 would be a fabricated number)\n");
    std::printf("      CPU frame    avg %.2f ms\n", s.avgCpuMs());
    // Both are ~0 while bgfx runs single-threaded (renderer.cpp calls
    // bgfx::renderFrame() before init on purpose). Printed regardless: they are
    // precisely what would price re-enabling the render thread.
    std::printf("      waits        submit %.2f ms   render %.2f ms%s\n",
                s.avgWaitSubmitMs(), s.avgWaitRenderMs(),
                (s.avgWaitSubmitMs() < 0.01 && s.avgWaitRenderMs() < 0.01)
                    ? "  (single-threaded bgfx — expected)" : "");
}

inline void printSubmitStats(const rdiag::SubmitStats& s, const char* tag) {
    std::printf(
        "[Submit] %s\n"
        "      items        %u considered, %u culled\n"
        "      draws        %u  (%u from submeshes, %u skinned)\n"
        "      batch runs   %u  -> instancing would remove %u submit(s)   [R5]\n"
        "      instanced    %u submit(s) covering %u item(s) -> %u submit(s) SAVED\n"
        "      material     %u bind(s) for %u draw(s)                     [R7]\n"
        "      bones        %u upload(s) for %u skinned item(s)           [R4]\n"
        "      shadow       %u draw(s) from %u item(s) (%u culled by the LIGHT frustum),\n"
        "                   %u bone upload(s)\n"
        "                   %u instanced submit(s) covering %u item(s)\n",
        tag, s.itemsConsidered, s.itemsCulled,
        s.draws, s.submeshDraws, s.skinnedDraws,
        s.batchRuns, s.instancingSavings(),
        s.instancedDraws, s.instancedItems,
        s.instancedItems > s.instancedDraws ? s.instancedItems - s.instancedDraws : 0,
        s.materialBinds, s.draws,
        s.bonePaletteUploads, s.skinnedItems,
        s.shadowDraws, s.shadowItemsConsidered, s.shadowItemsCulled,
        s.shadowBonePaletteUploads,
        s.shadowInstancedDraws, s.shadowInstancedItems);
    if (s.drawsDropped)
        std::printf("      DROPPED      %u draw(s) refused — frame INCOMPLETE "
                    "(backend uniform-buffer ceiling)\n", s.drawsDropped);
    // The R4 invariant, checked rather than described: one palette per ITEM.
    if (s.bonePaletteUploads != s.skinnedItems)
        std::printf("      WARNING: bone uploads (%u) != skinned items (%u) — "
                    "the palette is being re-uploaded per draw\n",
                    s.bonePaletteUploads, s.skinnedItems);
}

inline void printBudget(const BudgetReport& r) {
    std::printf("[Budget] target '%s' — %s\n", toString(r.tier),
                r.pass ? "PASS" : "OVER BUDGET");
    for (const auto& l : r.lines) {
        if (l.unit == BudgetUnit::Count)
            std::printf("      %-20s %8llu / %8llu      %3.0f%%  %s\n",
                        l.name, (unsigned long long)l.used,
                        (unsigned long long)l.limit, l.fraction() * 100.0,
                        l.ok ? "ok" : "OVER");
        else
            std::printf("      %-20s %8.1f / %8.1f MB   %3.0f%%  %s\n",
                        l.name, l.usedMb(), l.limitMb(), l.fraction() * 100.0,
                        l.ok ? "ok" : "OVER");
    }
    if (const auto* w = r.worst())
        std::printf("      worst: %s at %.0f%% of its ceiling\n",
                    w->name, w->fraction() * 100.0);
}

// Draw-call lines are counts, not bytes; printBudget's MB columns are wrong
// for them, so that line is rendered separately by callers that care.
inline void printCensus(const CensusReport& rep, const char* tag, size_t topN = 10) {
    const double mb = 1.0 / (1024.0 * 1024.0);
    std::printf("[Census] %s — %zu resources, %.1f MB resident\n",
                tag, rep.count, (double)rep.totalBytes * mb);
    size_t n = 0;
    for (const auto& r : rep.rows) {
        if (n++ >= topN) break;
        std::printf("      %8.2f MB  refs %-3u  %s\n",
                    (double)r.bytes * mb, r.refs, r.owner.c_str());
    }
}

inline void printOwnerCosts(const std::vector<OwnerCost>& costs, size_t topN = 10) {
    const double mb = 1.0 / (1024.0 * 1024.0);
    std::printf("[Census] cost by owner\n");
    size_t n = 0;
    for (const auto& o : costs) {
        if (n++ >= topN) break;
        std::printf("      %8.2f MB  %2zu resource(s)  %s\n",
                    (double)o.bytes * mb, o.resources, o.owner.c_str());
    }
}

inline void printLeakResult(const LeakResult& r, const char* tag) {
    const double mb = 1.0 / (1024.0 * 1024.0);
    if (r.clean()) {
        std::printf("[Leak] %s — clean (returned to baseline)\n", tag);
        return;
    }
    std::printf("[Leak] %s — LEAKED %.2f MB in %lld resource(s)\n",
                tag, (double)r.bytesDelta * mb, (long long)r.countDelta);
    for (const auto& row : r.stillReferenced)
        std::printf("      still referenced: %8.2f MB  refs %u  %s\n",
                    (double)row.bytes * mb, row.refs, row.owner.c_str());
}

} // namespace rdiag
