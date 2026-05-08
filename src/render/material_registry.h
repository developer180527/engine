#pragma once
#include <vector>
#include "core/handle.h"
#include "material.h"

class MaterialRegistry {
public:
    MaterialRegistry() {
        m_materials.emplace_back();  // slot 0 reserved
    }

    MaterialHandle addMaterial(Material m) {
        m_materials.push_back(std::move(m));
        return MaterialHandle{ static_cast<uint32_t>(m_materials.size() - 1) };
    }

    const Material* getMaterial(MaterialHandle h) const {
        if (!h.valid() || h.id >= m_materials.size()) return nullptr;
        return &m_materials[h.id];
    }

    void clear() { m_materials.clear(); }

private:
    std::vector<Material> m_materials;
};
