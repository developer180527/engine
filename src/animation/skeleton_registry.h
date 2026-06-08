#pragma once

#include "animation/skeleton.h"
#include "core/handle.h"

#include <cstdint>
#include <vector>

// Dense-vector registry for Skeleton assets. Same pattern as AssetRegistry
// (slot 0 reserved, free-list reuse). Skeletons are shared assets — many
// entities can point at the same one via SkeletonHandle.
class SkeletonRegistry {
public:
    SkeletonRegistry() {
        m_skeletons.emplace_back();  // slot 0 reserved (invalid handle)
    }

    SkeletonHandle add(Skeleton&& skel) {
        if (!m_freeSlots.empty()) {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            m_skeletons[slot] = std::move(skel);
            return SkeletonHandle{ slot };
        }
        m_skeletons.push_back(std::move(skel));
        return SkeletonHandle{ static_cast<uint32_t>(m_skeletons.size() - 1) };
    }

    bool remove(SkeletonHandle h) {
        if (!h.valid() || h.id >= m_skeletons.size()) return false;
        if (m_skeletons[h.id].bones.empty()) return false;  // already removed
        m_skeletons[h.id] = Skeleton{};
        m_freeSlots.push_back(h.id);
        return true;
    }

    const Skeleton* get(SkeletonHandle h) const {
        if (h.valid() && h.id < m_skeletons.size() && !m_skeletons[h.id].bones.empty())
            return &m_skeletons[h.id];
        return nullptr;
    }

    Skeleton* getMut(SkeletonHandle h) {
        if (h.valid() && h.id < m_skeletons.size() && !m_skeletons[h.id].bones.empty())
            return &m_skeletons[h.id];
        return nullptr;
    }

    size_t count() const { return m_skeletons.size() - 1 - m_freeSlots.size(); }

    void clear() {
        m_skeletons.clear();
        m_freeSlots.clear();
    }

private:
    std::vector<Skeleton> m_skeletons;
    std::vector<uint32_t> m_freeSlots;
};
