#pragma once
// ── SubmitStats — what the pipeline actually submitted, counted ───────────────
//
// ONE concern: make submission OBSERVABLE. Everything else about the renderer can
// be measured — bgfx reports handles, VRAM and GPU time; rworld's cull and sort
// are pure functions with their own tests — but what the pipeline *did* per view
// was invisible, and that had two costs.
//
// It made `src/render` untestable. bgfx's Noop backend never sets `numDraw` (its
// submit() writes timing, zeroes numPrims, and touches nothing else), so draw
// counts cannot be asserted headlessly against bgfx. Counting on OUR side works
// on any backend, including Noop — which is what lets a test assert submission
// behaviour with no GPU.
//
// And it made three audit findings unmeasurable, so they were argued from reading
// instead of numbers. Each now has a counter that states it directly:
//
//   R4 bone palette per submesh   `bonePaletteUploads` must equal `skinnedItems`,
//                                 never `skinnedDraws`. The invariant, as a number.
//   R7 redundant material binds   `materialBinds` vs `draws` IS the redundancy.
//   R5 no instancing              `batchRuns` vs `draws` IS the opportunity:
//                                 draws - batchRuns is what instancing would save.
//
// Deliberately a plain POD of counters, reset per view. No allocation, no
// virtuals, no strings: a diagnostic that perturbs the thing it measures is worse
// than none, and this sits in the submit loop.
#include <cstdint>

namespace rdiag {

struct SubmitStats {
    // ── Extraction / culling (mirrors rworld::VisibleSet, for one place to read)
    uint32_t itemsConsidered = 0;
    uint32_t itemsCulled     = 0;

    // ── Main pass ───────────────────────────────────────────────────────────
    uint32_t draws        = 0;   // bgfx::submit calls in the colour pass
    uint32_t submeshDraws = 0;   // of those, draws from a submesh range
    uint32_t skinnedItems = 0;   // items with a bone palette
    uint32_t skinnedDraws = 0;   // draws belonging to those items

    // R4's invariant lives here: one upload per skinned ITEM, not per draw.
    uint32_t bonePaletteUploads = 0;

    // R7: one bind per draw today. Equal to `draws` means no dedup is happening.
    uint32_t materialBinds = 0;

    // R5: runs of consecutive draws sharing mesh AND material (rworld::sameBatch).
    // `draws - batchRuns` is exactly the number of submits instancing removes.
    uint32_t batchRuns = 0;

    // ── Shadow pass ─────────────────────────────────────────────────────────
    // Counted separately because it has its own visibility story: it currently
    // walks EVERY item rather than a culled set, so `shadowDraws` growing with
    // scene size while `draws` stays flat is the shape of that bug.
    uint32_t shadowDraws            = 0;
    uint32_t shadowBonePaletteUploads = 0;

    // Draws the pipeline REFUSED to submit because the frame hit the backend's
    // uniform-buffer ceiling (see ForwardPipeline::kMaxDrawsPerFrame). Non-zero
    // means the frame is visibly incomplete — never silent.
    uint32_t drawsDropped = 0;

    void reset() { *this = SubmitStats{}; }

    // Convenience for readers, so the interpretation lives with the data.
    uint32_t instancingSavings() const {
        return draws > batchRuns ? draws - batchRuns : 0;
    }
};

} // namespace rdiag
