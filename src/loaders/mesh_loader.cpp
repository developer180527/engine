#include "loaders/mesh_loader.h"
#include "io/asset_storage.h"
#include "render/vertex.h"
#include <assetlib/mesh_asset.h>
#include <bgfx/bgfx.h>
#include <cstdio>
#include <cstring>

namespace MeshBinaryLoader {

MeshHandle load(const std::filesystem::path& cookedPath,
                const std::string&            sourcePath,
                AssetStorage&                 storage) {
    assetlib::MeshAsset asset;
    if (!assetlib::loadMesh(asset, cookedPath)) {
        std::fprintf(stderr, "[MeshLoader] Failed to load: %s\n",
                     cookedPath.string().c_str());
        return {};
    }

    const auto& h = asset.header;

    // Sanity check: cooked stride must match the runtime Vertex layout.
    // If they differ, the cook format and vertex.h are out of sync.
    if (h.vertexStride != sizeof(Vertex)) {
        std::fprintf(stderr,
            "[MeshLoader] Stride mismatch: cooked=%u runtime=%zu in %s\n",
            h.vertexStride, sizeof(Vertex), cookedPath.string().c_str());
        return {};
    }

    // Vertex buffer — raw bytes map directly to Vertex layout, zero conversion
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(
        bgfx::copy(asset.vertexData.data(),
                   static_cast<uint32_t>(asset.vertexData.size())),
        Vertex::layout());

    // Index buffer — always uint32 from cook pipeline
    const bool use32 = (h.indexStride == 4);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
        bgfx::copy(asset.indexData.data(),
                   static_cast<uint32_t>(asset.indexData.size())),
        use32 ? BGFX_BUFFER_INDEX32 : BGFX_BUFFER_NONE);

    Mesh mesh(vbh, ibh, h.indexCount);
    mesh.sourcePath  = sourcePath;
    mesh.boundsMin   = { h.boundsMin[0], h.boundsMin[1], h.boundsMin[2] };
    mesh.boundsMax   = { h.boundsMax[0], h.boundsMax[1], h.boundsMax[2] };
    mesh.doubleSided = false; // cooked geometry has correct winding

    std::printf("[MeshLoader] Loaded %s  verts=%u idx=%u\n",
                cookedPath.filename().string().c_str(),
                h.vertexCount, h.indexCount);

    return storage.meshes.addMesh(std::move(mesh));
}

} // namespace MeshBinaryLoader
