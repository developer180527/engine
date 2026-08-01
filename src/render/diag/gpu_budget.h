#pragma once
// ── GpuBudget — "does this fit the machine we ship to?" ─────────────────────
//
// ONE concern: policy. Turning a measurement into a verdict.
//
// A number without a limit is trivia. `71 MB of VRAM` means nothing until you
// know the target has 128 MB — and that is exactly the mistake this engine
// already made: a hardcoded 4096² shadow map cost 64 MB, 90% of a shipped
// frame's GPU memory, and nothing in the build had an opinion about it.
//
// Deliberately free of bgfx and of FrameGpuStats' collection logic, so the
// policy is pure data + arithmetic and can be unit-tested with no GPU.
#include <cstdint>
#include <vector>

namespace rdiag {

class FrameGpuStats;

// The hardware class a project targets. `Low` is not hypothetical: it is the
// Intel UHD 630 / 4 GB machine this engine is being validated against, and it
// is the tier that decides most architecture (see docs/architecture/renderer-architecture.md
// §2 — it is why the renderer is forward, not deferred).
enum class TargetTier { Low, Mid, High };

const char* toString(TargetTier t);
// Parses "low"/"mid"/"high" (case-insensitive). Unknown -> Low, because
// guessing generously is how a budget check stops catching anything.
TargetTier  parseTier(const char* s);

struct GpuBudget {
    uint64_t totalVramBytes    = 0;   // textures + render targets
    uint64_t textureBytes      = 0;
    uint64_t renderTargetBytes = 0;
    uint32_t maxDraws          = 0;

    static GpuBudget forTier(TargetTier t);
};

// Not every budget is memory: draw calls are a count, and formatting them as
// megabytes renders "13 / 500" as "0.0 / 0.0 MB" — a line that looks broken
// and tells the reader nothing.
enum class BudgetUnit { Bytes, Count };

// One measured quantity against its ceiling.
struct BudgetLine {
    const char* name  = "";
    uint64_t    used  = 0;
    uint64_t    limit = 0;
    bool        ok    = true;
    BudgetUnit  unit  = BudgetUnit::Bytes;
    double usedMb()  const { return (double)used  / (1024.0 * 1024.0); }
    double limitMb() const { return (double)limit / (1024.0 * 1024.0); }
    // How much of the budget this line consumes; > 1.0 means over.
    double fraction() const { return limit ? (double)used / (double)limit : 0.0; }
};

struct BudgetReport {
    TargetTier              tier = TargetTier::Low;
    std::vector<BudgetLine> lines;
    bool                    pass = true;

    // The line furthest over budget (or closest to it) — what to fix first.
    const BudgetLine* worst() const;
};

// Evaluate collected stats against a tier's ceilings.
BudgetReport evaluate(const FrameGpuStats& stats, TargetTier tier);

} // namespace rdiag
