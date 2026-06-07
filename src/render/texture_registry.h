#pragma once
#include <vector>
#include "core/handle.h"
#include "texture.h"

class TextureRegistry {
public:
    TextureRegistry() {
        m_textures.emplace_back();  // slot 0 reserved — maps to invalid handle
    }

    // Register a texture. Reuses freed slots when available; otherwise appends.
    TextureHandle addTexture(Texture t) {
        if (!m_freeSlots.empty()) {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            m_textures[slot] = std::move(t);
            return TextureHandle{ slot };
        }
        m_textures.push_back(std::move(t));
        return TextureHandle{ static_cast<uint32_t>(m_textures.size() - 1) };
    }

    // Release a texture and return its slot to the free list.
    // The Texture destructor handles GPU resource cleanup via RAII.
    bool removeTexture(TextureHandle h) {
        if (!h.valid() || h.id >= m_textures.size()) return false;
        if (!m_textures[h.id].valid()) return false;  // already removed
        m_textures[h.id] = Texture{};   // RAII releases bgfx texture
        m_freeSlots.push_back(h.id);
        return true;
    }

    const Texture* getTexture(TextureHandle h) const {
        if (!h.valid() || h.id >= m_textures.size()) return nullptr;
        return m_textures[h.id].valid() ? &m_textures[h.id] : nullptr;
    }

    // Number of live (non-removed) textures, excluding reserved slot 0.
    size_t textureCount() const {
        return m_textures.size() - 1 - m_freeSlots.size();
    }

    void clear() {
        m_textures.clear();
        m_freeSlots.clear();
    }

private:
    std::vector<Texture>  m_textures;
    std::vector<uint32_t> m_freeSlots;  // Indices of removed slots available for reuse.
};
