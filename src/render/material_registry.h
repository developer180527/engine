#pragma once
#include <vector>
#include "core/handle.h"
#include "material.h"

class MaterialRegistry {
public:
    MaterialRegistry() {
        m_materials.emplace_back();  // slot 0 reserved
        m_occupied.push_back(false); // slot 0 is not a real material
    }

    // Register a material. Reuses freed slots when available; otherwise appends.
    MaterialHandle addMaterial(Material m) {
        if (!m_freeSlots.empty()) {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            m_materials[slot] = std::move(m);
            m_occupied[slot]  = true;
            return MaterialHandle{ slot };
        }
        m_materials.push_back(std::move(m));
        m_occupied.push_back(true);
        return MaterialHandle{ static_cast<uint32_t>(m_materials.size() - 1) };
    }

    // Release a material and return its slot to the free list.
    // Material has no GPU resources — this just frees the slot for reuse.
    bool removeMaterial(MaterialHandle h) {
        if (!h.valid() || h.id >= m_materials.size()) return false;
        if (!m_occupied[h.id]) return false;  // already removed
        m_materials[h.id] = Material{};
        m_occupied[h.id]  = false;
        m_freeSlots.push_back(h.id);
        return true;
    }

    const Material* getMaterial(MaterialHandle h) const {
        if (!h.valid() || h.id >= m_materials.size()) return nullptr;
        if (!m_occupied[h.id]) return nullptr;
        return &m_materials[h.id];
    }

    // Number of live (non-removed) materials, excluding reserved slot 0.
    size_t materialCount() const {
        return m_materials.size() - 1 - m_freeSlots.size();
    }

    void clear() {
        m_materials.clear();
        m_occupied.clear();
        m_freeSlots.clear();
    }

private:
    std::vector<Material> m_materials;
    std::vector<bool>     m_occupied;   // Parallel to m_materials; needed because Material lacks valid().
    std::vector<uint32_t> m_freeSlots;  // Indices of removed slots available for reuse.
};
