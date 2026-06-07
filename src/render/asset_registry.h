#pragma once

#include "mesh.h"
#include "core/handle.h"

#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>

// AssetRegistry owns all loaded mesh assets and hands out handles.
//
// Components that want to use a mesh hold a MeshHandle (an 8-byte index)
// rather than the Mesh itself. This means many entities can share one mesh
// without duplicating GPU resources, and asset lifetime is centrally
// controlled.
//
// Slot reuse: when a mesh is removed via removeMesh(), its slot index is
// pushed onto a free list. The next addMesh() pops from the free list
// instead of growing the vector, keeping handle indices dense and avoiding
// unbounded growth during load/unload cycles.
//
// The "missing mesh" placeholder pattern: getMesh() returns nullptr for
// invalid or removed handles. Missing meshes will eventually be visualized
// as a magenta cube to make the problem visually obvious.
class AssetRegistry {
public:
    AssetRegistry() {
        m_meshes.emplace_back();  // slot 0 reserved — maps to invalid handle
    }

    // Register a mesh. Takes ownership of the GPU buffers via Mesh's move
    // semantics. Returns a handle that can be used to retrieve the mesh later.
    // Reuses freed slots when available; otherwise appends.
    // Index 0 is reserved as "invalid" (matches Handle's default).
    MeshHandle addMesh(Mesh&& mesh) {
        if (!m_freeSlots.empty()) {
            uint32_t slot = m_freeSlots.back();
            m_freeSlots.pop_back();
            m_meshes[slot] = std::move(mesh);
            return MeshHandle{ slot };
        }
        m_meshes.push_back(std::move(mesh));
        return MeshHandle{ static_cast<uint32_t>(m_meshes.size() - 1) };
    }

    // Release a mesh and return its slot to the free list.
    // The Mesh destructor handles GPU resource cleanup via RAII.
    // Returns true if the mesh was actually removed, false if the handle
    // was invalid or already removed.
    bool removeMesh(MeshHandle h) {
        if (!h.valid() || h.id >= m_meshes.size()) return false;
        if (!m_meshes[h.id].valid()) return false;  // already removed
        m_meshes[h.id] = Mesh{};   // RAII releases bgfx buffers
        m_freeSlots.push_back(h.id);
        return true;
    }

    // Retrieve a mesh by handle. Returns nullptr if the handle is invalid,
    // out of range, or was removed.
    const Mesh* getMesh(MeshHandle h) const {
        if (h.id < m_meshes.size() && m_meshes[h.id].valid()) {
            return &m_meshes[h.id];
        }
        return nullptr;  // TODO: return placeholder mesh once we build one
    }

    // Number of live (non-removed) meshes, excluding reserved slot 0.
    size_t meshCount() const {
        return m_meshes.size() - 1 - m_freeSlots.size();
    }

    // Free all GPU resources and reset the free list. Called at engine shutdown.
    // Mesh destructors do the actual work via RAII.
    void clear() {
        m_meshes.clear();
        m_freeSlots.clear();
    }

private:
    std::vector<Mesh>     m_meshes;     // Index = handle id. Slot 0 is unused.
    std::vector<uint32_t> m_freeSlots;  // Indices of removed slots available for reuse.
};
