#pragma once

#include "animation/animation_clip.h"
#include "core/handle.h"

#include <cstdint>
#include <vector>

// Dense-vector registry for AnimClip assets. Same pattern as AssetRegistry
// (slot 0 reserved, free-list reuse). Clips are shared assets — many
// Animator components can reference the same clip.
class AnimClipRegistry {
public:
    AnimClipRegistry() {
        m_clips.emplace_back();  // slot 0 reserved (invalid handle)
    }

    AnimClipHandle add(AnimClip&& clip) {
        if (!m_freeSlots.empty()) {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            m_clips[slot] = std::move(clip);
            return AnimClipHandle{ slot };
        }
        m_clips.push_back(std::move(clip));
        return AnimClipHandle{ static_cast<uint32_t>(m_clips.size() - 1) };
    }

    bool remove(AnimClipHandle h) {
        if (!h.valid() || h.id >= m_clips.size()) return false;
        if (m_clips[h.id].channels.empty() && m_clips[h.id].name.empty()) return false;
        m_clips[h.id] = AnimClip{};
        m_freeSlots.push_back(h.id);
        return true;
    }

    const AnimClip* get(AnimClipHandle h) const {
        if (h.valid() && h.id < m_clips.size() &&
            !(m_clips[h.id].channels.empty() && m_clips[h.id].name.empty()))
            return &m_clips[h.id];
        return nullptr;
    }

    size_t count() const { return m_clips.size() - 1 - m_freeSlots.size(); }

    void clear() {
        m_clips.clear();
        m_freeSlots.clear();
    }

private:
    std::vector<AnimClip> m_clips;
    std::vector<uint32_t> m_freeSlots;
};
