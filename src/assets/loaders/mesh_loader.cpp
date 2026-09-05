#include "assets/loaders/mesh_loader.h"
#include "assets/asset_storage.h"
#include "render/gpu.h"
#include "render/vertex.h"
#include <assetlib/mesh_asset.h>
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

    // Index stride must be a real width. A corrupt header value (0/1/3) would
    // otherwise fall through to a silent 16-bit interpretation, scrambling the
    // primitive stream (asset audit: "Destructive Strided Index Fallback").
    if (h.indexStride != 2 && h.indexStride != 4) {
        std::fprintf(stderr, "[MeshLoader] Bad index stride %u in %s\n",
                     h.indexStride, cookedPath.string().c_str());
        return {};
    }

    // The header's declared counts MUST match the actual byte payloads. A
    // truncated .cooked file would let the staging copy upload a buffer smaller than
    // indexCount claims, and the GPU reads out of bounds at draw time — a
    // driver timeout, not a clean failure (asset audit: "Missing Vector Size
    // and Bound Validation").
    if (asset.vertexData.size() != (size_t)h.vertexCount * h.vertexStride ||
        asset.indexData.size()  != (size_t)h.indexCount  * h.indexStride) {
        std::fprintf(stderr,
            "[MeshLoader] Payload size mismatch (truncated/corrupt?) in %s: "
            "verts %zu vs %zu, idx %zu vs %zu\n",
            cookedPath.string().c_str(),
            asset.vertexData.size(), (size_t)h.vertexCount * h.vertexStride,
            asset.indexData.size(),  (size_t)h.indexCount  * h.indexStride);
        return {};
    }

    // Vertex buffer — raw bytes map directly to Vertex layout, zero conversion
    gpu::VertexBufferHandle vbh = gpu::createVertexBuffer(
        gpu::copy(asset.vertexData.data(),
                  static_cast<uint32_t>(asset.vertexData.size())),
        gpu::VertexFormat::Standard);

    // Index buffer — always uint32 from cook pipeline
    const bool use32 = (h.indexStride == 4);
    gpu::IndexBufferHandle ibh = gpu::createIndexBuffer(
        gpu::copy(asset.indexData.data(),
                  static_cast<uint32_t>(asset.indexData.size())),
        use32 ? gpu::IndexFormat::U32 : gpu::IndexFormat::U16);

    // Never register invalid handles — a failed GPU allocation would otherwise
    // crash downstream at draw time (asset audit: "Unchecked GPU Buffer
    // Allocations"). This is also the headless path: with no device every
    // create returns invalid, so the parse above still runs and validates the
    // file, and only the upload is skipped.
    if (!vbh.valid() || !ibh.valid()) {
        gpu::destroy(vbh);
        gpu::destroy(ibh);
        std::fprintf(stderr, "[MeshLoader] GPU buffer creation failed for %s\n",
                     cookedPath.string().c_str());
        return {};
    }

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
