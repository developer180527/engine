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
// Slots are recycled through a free list. Acquisition is lazy (the animator
// takes one the first time it writes an entity), and release happens from the
// SkinnedMesh remove hook — see AnimatorSystem::init. An entity destroyed in a
// world where that hook was never registered leaks its slot: bounded, never
// unsafe, and it costs memory rather than correctness.
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

    uint32_t acquire() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_free.empty()) {
            const uint32_t slot = m_free.back();
            m_free.pop_back();
            return slot;
        }
        const uint32_t slot = m_next++;
        const uint32_t chunk = slot / kChunkSlots;
        while (m_chunks.size() <= chunk)
            m_chunks.push_back(std::make_unique<float[]>(
                (size_t)kChunkSlots * kFloats));
        return slot;
    }

    void release(uint32_t slot) {
        if (slot == kNoSlot) return;
        std::lock_guard<std::mutex> lk(m_mtx);
        if (slot < m_next) m_free.push_back(slot);
    }

    // Stable for the lifetime of the slot. Null for kNoSlot so callers can
    // pass an unacquired component straight through.
    float* at(uint32_t slot) {
        if (slot == kNoSlot) return nullptr;
        const uint32_t chunk = slot / kChunkSlots;
        // No lock: chunks are never moved or freed once created, and a slot is
        // owned by exactly one entity, so two threads never touch one palette.
        // Only the CHUNK VECTOR is shared, and acquire() only appends — but a
        // concurrent append can reallocate the vector of unique_ptrs, so the
        // read of m_chunks itself is guarded.
        std::lock_guard<std::mutex> lk(m_mtx);
        if (chunk >= m_chunks.size()) return nullptr;
        return m_chunks[chunk].get() + (size_t)(slot % kChunkSlots) * kFloats;
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
    std::vector<std::unique_ptr<float[]>>   m_chunks;
    std::vector<uint32_t>                   m_free;
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
