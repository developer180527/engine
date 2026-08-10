#pragma once
// ── SkinPalettePool — bone palettes, out of the ECS component ────────────────
//
// A bone palette is mat4[128] = 8 KB. It used to live INSIDE the SkinnedMesh
// component, and that made the component 8 200 bytes — 64 cache lines, more
// than a page.
//
// The cost was not in the animation. It was in the ITERATION: the renderer's
// extraction query takes SkinnedMesh as a term and reads five bytes of it per
// entity (`skeleton`, `hasSkinMatrices`), so every byte the component carried
// was stride the renderer paid for and never looked at. Measured on the real
// component at 20 000 entities, reading the handle cost 15.8x more with the
// palette inline than with it out of line, and the ratio grew with entity
// count — the signature of falling out of cache rather than of doing work.
//
// So the palette moves here and the component keeps a SLOT. Storage is
// CHUNKED and never reallocated: a pointer handed to the renderer must stay
// valid while the GPU upload reads it, and a std::vector that grows would
// invalidate every outstanding pointer at the worst possible moment.
//
// ── at() TAKES NO LOCK, AND THAT IS THE WHOLE POINT ─────────────────────────
// The first version guarded `at()` with the pool mutex to make reading the
// chunk vector safe. That is correct and it is also a serialization point in
// the one place that must not have one: `Renderer::buildView` calls `at()` once
// per skinned item from inside `jobs::parallelFor`, so every worker thread
// queued on a single mutex — on the exact path this class exists to speed up.
//
// The fix is a published pointer table rather than a lock. Chunk pointers live
// in a fixed-capacity array of atomics: `acquire` allocates a chunk and
// releases the pointer into the table, `at` acquires it. The vector of
// unique_ptrs stays behind the mutex for OWNERSHIP only and is never read on
// the hot path. Fixed capacity is what makes the table safe to index without a
// lock; kMaxChunks * kChunkSlots is the hard palette ceiling, and running out
// returns kNoSlot, which every caller already handles (the animator clears
// hasSkinMatrices and the item draws unskinned rather than wrong).
//
// Slots are recycled through a free list. Acquisition is lazy (the animator
// takes one the first time it writes an entity), and release happens from the
// SkinnedMesh remove hook — which AnimatorSystem installs on EVERY world it
// ticks, not just the one it was init()ed with. Component hooks are world
// state, so registering only on the editor world leaked one palette per
// animated entity per play session, forever.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace anim {

class SkinPalettePool {
public:
    static constexpr int      kMaxBones    = 128;
    static constexpr int      kFloats      = kMaxBones * 16;   // mat4[128]
    static constexpr uint32_t kNoSlot      = 0xFFFFFFFFu;
    // Palettes per chunk. Chunked so a growing pool never moves the palettes
    // already handed out.
    static constexpr uint32_t kChunkSlots  = 64;
    // 1024 chunks x 64 palettes x 8 KB = 512 MB of skinning, which is far past
    // anything shippable — the cap exists to make the pointer table a
    // fixed-size array (see the note above), not to ration memory.
    static constexpr uint32_t kMaxChunks   = 1024;
    static constexpr uint32_t kMaxSlots    = kMaxChunks * kChunkSlots;

    uint32_t acquire() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_free.empty()) {
            const uint32_t slot = m_free.back();
            m_free.pop_back();
            m_isFree[slot] = 0;
            return slot;
        }
        if (m_next >= kMaxSlots) return kNoSlot;    // ceiling; caller degrades
        const uint32_t slot  = m_next++;
        const uint32_t chunk = slot / kChunkSlots;
        while (m_chunks.size() <= chunk) {
            auto block = std::make_unique<float[]>((size_t)kChunkSlots * kFloats);
            // PUBLISH LAST. The raw pointer becomes visible to a lock-free at()
            // the moment this store lands, so the allocation must be complete
            // first — release/acquire is what orders that.
            m_chunkPtr[m_chunks.size()].store(block.get(), std::memory_order_release);
            m_chunks.push_back(std::move(block));
        }
        m_isFree.resize(m_next, 0);
        m_isFree[slot] = 0;
        return slot;
    }

    void release(uint32_t slot) {
        if (slot == kNoSlot) return;
        std::lock_guard<std::mutex> lk(m_mtx);
        if (slot >= m_next) return;
        // A DOUBLE RELEASE MUST BE A NO-OP. Pushing a slot onto the free list
        // twice hands the same palette to two entities, which is not a leak but
        // silent corruption: two skeletons writing one buffer, and whichever
        // animated last wins for both. Cheaper to make impossible than to debug.
        if (m_isFree[slot]) return;
        m_isFree[slot] = 1;
        m_free.push_back(slot);
    }

    // Stable for the lifetime of the slot. Null for kNoSlot so callers can
    // pass an unacquired component straight through.
    //
    // LOCK-FREE: reads one atomic chunk pointer. Chunks are never moved and
    // never freed, and a slot belongs to exactly one entity, so no two threads
    // touch one palette. See the header note for why this matters.
    float* at(uint32_t slot) const {
        if (slot == kNoSlot || slot >= kMaxSlots) return nullptr;
        float* base = m_chunkPtr[slot / kChunkSlots].load(std::memory_order_acquire);
        if (!base) return nullptr;
        return base + (size_t)(slot % kChunkSlots) * kFloats;
    }

    size_t slotsInUse() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return (size_t)m_next - m_free.size();
    }
    size_t bytesResident() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_chunks.size() * (size_t)kChunkSlots * kFloats * sizeof(float);
    }

private:
    mutable std::mutex                      m_mtx;
    std::vector<std::unique_ptr<float[]>>   m_chunks;   // ownership only
    // The hot-path view of the same memory. Written under m_mtx, read without.
    std::atomic<float*>                     m_chunkPtr[kMaxChunks] = {};
    std::vector<uint32_t>                   m_free;
    std::vector<uint8_t>                    m_isFree;   // free-list membership
    uint32_t                                m_next = 0;
};

// Process-wide, like the profiler: the pool is referenced from the animator
// (writer), the renderer's extraction (reader) and the component remove hook,
// and threading a pointer through all three — including across the kit C API —
// would buy nothing. Palettes are runtime-only state that never serializes.
inline SkinPalettePool& skinPalettes() {
    static SkinPalettePool pool;
    return pool;
}

} // namespace anim
