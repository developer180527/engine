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
// The "missing mesh" placeholder pattern: getMesh() never returns null. If
// a caller asks for a mesh that doesn't exist (invalid handle, deleted asset),
// they get a fallback "missing" mesh. This means the renderer can iterate
// entities without per-entity null checks. Missing meshes will eventually
// be visualized as a magenta cube to make the problem visually obvious.
//
// Currently we don't yet build the placeholder — it's stubbed out. The
// pattern is in place; the implementation will be filled in as the engine
// matures.
class AssetRegistry {
public:
    // Register a mesh. Takes ownership of the GPU buffers via Mesh's move
    // semantics. Returns a handle that can be used to retrieve the mesh later.
    // Index 0 is reserved as "invalid" (matches Handle's default).
    MeshHandle addMesh(Mesh&& mesh) {
        // First valid handle is index 1 (id=0 is reserved as invalid).
        if (m_meshes.empty()) {
            m_meshes.emplace_back(); // Slot 0 stays empty (the "invalid" slot)
        }
        m_meshes.push_back(std::move(mesh));
        return MeshHandle{ static_cast<uint32_t>(m_meshes.size() - 1) };
    }

    // Retrieve a mesh by handle. Returns the missing-mesh placeholder if the
    // handle is invalid or out of range. Never returns null.
    const Mesh* getMesh(MeshHandle h) const {
        if (h.id < m_meshes.size() && m_meshes[h.id].valid()) {
            return &m_meshes[h.id];
        }
        return nullptr;  // TODO: return placeholder mesh once we build one
    }

    // Total number of registered meshes (excluding slot 0).
    size_t meshCount() const {
        return m_meshes.empty() ? 0 : m_meshes.size() - 1;
    }

    // Free all GPU resources. Called at engine shutdown.
    // Mesh destructors do the actual work via RAII.
    void clear() {
        m_meshes.clear();
    }

private:
    std::vector<Mesh> m_meshes;  // Index = handle id. Slot 0 is unused.
};
