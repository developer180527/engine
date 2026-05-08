#pragma once
#include <vector>
#include "core/handle.h"
#include "texture.h"

class TextureRegistry {
public:
    TextureRegistry() {
        m_textures.emplace_back();  // slot 0 reserved — maps to invalid handle
    }

    TextureHandle addTexture(Texture t) {
        m_textures.push_back(std::move(t));
        return TextureHandle{ static_cast<uint32_t>(m_textures.size() - 1) };
    }

    const Texture* getTexture(TextureHandle h) const {
        if (!h.valid() || h.id >= m_textures.size()) return nullptr;
        return m_textures[h.id].valid() ? &m_textures[h.id] : nullptr;
    }

    void clear() { m_textures.clear(); }

private:
    std::vector<Texture> m_textures;
};
