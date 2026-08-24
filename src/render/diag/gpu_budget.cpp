#include "render/diag/gpu_budget.h"
#include "render/diag/frame_gpu_stats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace rdiag {
namespace {
constexpr uint64_t MB = 1024ull * 1024ull;
}

const char* toString(TargetTier t) {
    switch (t) {
        case TargetTier::Low:  return "low";
        case TargetTier::Mid:  return "mid";
        case TargetTier::High: return "high";
    }
    return "?";
}

TargetTier parseTier(const char* s) {
    if (!s) return TargetTier::Low;
    std::string v;
    for (const char* p = s; *p; ++p) v += (char)std::tolower((unsigned char)*p);
    if (v == "mid"  || v == "medium") return TargetTier::Mid;
    if (v == "high" || v == "ultra")  return TargetTier::High;
    return TargetTier::Low;   // unknown -> strictest; see the header
}

GpuBudget GpuBudget::forTier(TargetTier t) {
    GpuBudget b;
    switch (t) {
    case TargetTier::Low:
        // Intel UHD 630, ~128 MB usable of shared memory, 4 GB system RAM.
        // The sub-limits leave deliberate headroom: they sum to 80 MB, not
        // 128, because a budget with no slack is a budget you blow on the
        // first feature. Draw ceiling is conservative for an iGPU without
        // instancing (today's scene submits ~13).
        b.totalVramBytes    = 128 * MB;
        b.textureBytes      =  60 * MB;
        b.renderTargetBytes =  20 * MB;
        b.maxDraws          = 500;
        break;
    case TargetTier::Mid:
        b.totalVramBytes    = 2048 * MB;
        b.textureBytes      = 1024 * MB;
        b.renderTargetBytes =  256 * MB;
        b.maxDraws          = 3000;
        break;
    case TargetTier::High:
        b.totalVramBytes    = 8192 * MB;
        b.textureBytes      = 4096 * MB;
        b.renderTargetBytes = 1024 * MB;
        b.maxDraws          = 10000;
        break;
    }
    return b;
}

BudgetReport evaluate(const FrameGpuStats& stats, TargetTier tier) {
    const GpuBudget b = GpuBudget::forTier(tier);
    BudgetReport r;
    r.tier = tier;

    auto add = [&](const char* name, uint64_t used, uint64_t limit,
                   BudgetUnit unit = BudgetUnit::Bytes) {
        BudgetLine l{name, used, limit, used <= limit, unit};
        if (!l.ok) r.pass = false;
        r.lines.push_back(l);
    };

    const auto tex = (uint64_t)std::max<int64_t>(0, stats.textureBytes());
    const auto rt  = (uint64_t)std::max<int64_t>(0, stats.rtBytes());

    // PEAK, not current, for the total: a frame that briefly exceeds VRAM
    // still stutters or fails to allocate. Steady-state is not the constraint.
    add("total VRAM (peak)", (uint64_t)std::max<int64_t>(0, stats.peakVramBytes()),
        b.totalVramBytes);
    add("textures",          tex, b.textureBytes);
    add("render targets",    rt,  b.renderTargetBytes);
    add("draw calls (max)",  stats.maxDraws(), b.maxDraws, BudgetUnit::Count);
    return r;
}

const BudgetLine* BudgetReport::worst() const {
    if (lines.empty()) return nullptr;
    const BudgetLine* w = &lines.front();
    for (const auto& l : lines)
        if (l.fraction() > w->fraction()) w = &l;
    return w;
}

} // namespace rdiag
